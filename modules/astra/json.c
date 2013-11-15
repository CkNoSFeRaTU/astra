/*
 * Astra Module: JSON
 * http://cesbo.com/astra
 *
 * Copyright (C) 2012-2013, Andrey Dyldin <and@cesbo.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <astra.h>

/*
 * ooooooooooo oooo   oooo  oooooooo8   ooooooo  ooooooooo  ooooooooooo
 *  888    88   8888o  88 o888     88 o888   888o 888    88o 888    88
 *  888ooo8     88 888o88 888         888     888 888    888 888ooo8
 *  888    oo   88   8888 888o     oo 888o   o888 888    888 888    oo
 * o888ooo8888 o88o    88  888oooo88    88ooo88  o888ooo88  o888ooo8888
 *
 */

#define MAX_BUFFER_SIZE 4096

typedef struct string_buffer_t string_buffer_t;

struct string_buffer_t
{
    char buffer[MAX_BUFFER_SIZE];
    int size;

    string_buffer_t *last;
    string_buffer_t *next;
};

static void string_buffer_addchar(string_buffer_t *buffer, char c)
{
    string_buffer_t *last = buffer->last;
    if(last->size + 1 > MAX_BUFFER_SIZE)
    {
        last->next = malloc(sizeof(string_buffer_t));
        last = last->next;
        last->size = 0;
        last->last = NULL;
        last->next = NULL;
        buffer->last = last;
    }

    last->buffer[last->size] = c;
    ++last->size;
}

static void string_buffer_addlstring(string_buffer_t *buffer, const char *str, int size)
{
    string_buffer_t *last = buffer->last;

    if(last->size + size > MAX_BUFFER_SIZE)
    {
        const int cap = MAX_BUFFER_SIZE - last->size;
        if(cap > 0)
        {
            memcpy(&last->buffer[last->size], str, cap);
            last->size += cap;
        }

        last->next = malloc(sizeof(string_buffer_t));
        last = last->next;
        last->size = 0;
        last->last = NULL;
        last->next = NULL;
        buffer->last = last;
        string_buffer_addlstring(buffer, &str[cap], size - cap);
        return;
    }
    else
    {
        memcpy(&last->buffer[last->size], str, size);
        last->size += size;
    }
}

static void string_buffer_push(lua_State *L, string_buffer_t *buffer)
{
    luaL_Buffer b;
    luaL_buffinit(L, &b);

    string_buffer_t *next_next;
    for(string_buffer_t *next = buffer
        ; next && (next_next = next->next, 1)
        ; next = next_next)
    {
        luaL_addlstring(&b, next->buffer, next->size);
        free(next);
    }

    luaL_pushresult(&b);
}

static void walk_table(lua_State *L, string_buffer_t *buffer);

static void set_string(string_buffer_t *buffer, const char *str)
{
    char c;
    string_buffer_addchar(buffer, '"');
    for(int i = 0; (c = str[i]) != '\0'; ++i)
    {
        switch(c)
        {
            case '\\':
                string_buffer_addlstring(buffer, "\\\\", 2);
                break;
            case '"':
                string_buffer_addlstring(buffer, "\\\"", 2);
                break;
            case '\t':
                string_buffer_addlstring(buffer, "\\t", 2);
                break;
            case '\r':
                string_buffer_addlstring(buffer, "\\r", 2);
                break;
            case '\n':
                string_buffer_addlstring(buffer, "\\n", 2);
                break;
            default:
                string_buffer_addchar(buffer, c);
                break;
        }
    }
    string_buffer_addchar(buffer, '"');
}

static void set_value(lua_State *L, string_buffer_t *buffer)
{
    switch(lua_type(L, -1))
    {
        case LUA_TTABLE:
        {
            walk_table(L, buffer);
            break;
        }
        case LUA_TBOOLEAN:
        {
            if(lua_toboolean(L, -1) == true)
                string_buffer_addlstring(buffer, "true", 4);
            else
                string_buffer_addlstring(buffer, "false", 5);
            break;
        }
        case LUA_TNUMBER:
        {
            char number[32];
            const int size = snprintf(number, sizeof(number), "%.14g", lua_tonumber(L, -1));
            string_buffer_addlstring(buffer, number, size);
            break;
        }
        case LUA_TSTRING:
        {
            set_string(buffer, lua_tostring(L, -1));
            break;
        }
        default:
            break;
    }
}

static void walk_table(lua_State *L, string_buffer_t *buffer)
{
    luaL_checktype(L, -1, LUA_TTABLE);

    int pairs_count = 0;
    for(lua_pushnil(L); lua_next(L, -2); lua_pop(L, 1))
        ++pairs_count;

    const bool is_array = (luaL_len(L, -1) == pairs_count);
    bool is_first = true;



    if(is_array)
    {
        string_buffer_addchar(buffer, '[');

        for(lua_pushnil(L); lua_next(L, -2); lua_pop(L, 1))
        {
            if(!is_first)
                string_buffer_addchar(buffer, ',');
            else
                is_first = false;

            set_value(L, buffer);
        }

        string_buffer_addchar(buffer, ']');
    }
    else
    {
        string_buffer_addchar(buffer, '{');

        for(lua_pushnil(L); lua_next(L, -2); lua_pop(L, 1))
        {
            if(!is_first)
                string_buffer_addchar(buffer, ',');
            else
                is_first = false;

            set_string(buffer, lua_tostring(L, -2));
            string_buffer_addchar(buffer, ':');
            set_value(L, buffer);
        }

        string_buffer_addchar(buffer, '}');
    }
}

static int json_encode(lua_State *L)
{
    luaL_checktype(L, -1, LUA_TTABLE);

    string_buffer_t *buffer = malloc(sizeof(string_buffer_t));
    buffer->size = 0;
    buffer->last = buffer;
    buffer->next = NULL;

    walk_table(L, buffer);

    string_buffer_push(L, buffer);
    return 1;
}

/*
 * ooooooooo  ooooooooooo  oooooooo8   ooooooo  ooooooooo  ooooooooooo
 *  888    88o 888    88 o888     88 o888   888o 888    88o 888    88
 *  888    888 888ooo8   888         888     888 888    888 888ooo8
 *  888    888 888    oo 888o     oo 888o   o888 888    888 888    oo
 * o888ooo88  o888ooo8888 888oooo88    88ooo88  o888ooo88  o888ooo8888
 *
 */

static int skip_sp(const char *str, int pos)
{
    do
    {
        switch(str[pos])
        {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                ++pos;
                continue;
            case '\0':
            default:
                return pos;
        }
    } while(true);
}

static int skip_comment(const char *str, int pos)
{
    char c;
    for(; (c = str[pos]) != '\0'; ++pos)
    {
        if(c == '*' && str[pos + 1] == '/')
            return pos + 2;
    }

    return -1;
}

static int scan_string(lua_State *L, const char *str, int pos)
{
    char c;
    luaL_Buffer b;
    luaL_buffinit(L, &b);

    for(; (c = str[pos]) != '\0'; ++pos)
    {
        if(c == '"')
            break;

        else if(c == '\\')
        {
            ++pos;
            switch(str[pos])
            {
                case '\\':
                    luaL_addchar(&b, '\\');
                    break;
                case '"':
                    luaL_addchar(&b, '"');
                    break;
                case 't':
                    luaL_addchar(&b, '\t');
                    break;
                case 'r':
                    luaL_addchar(&b, '\r');
                    break;
                case 'n':
                    luaL_addchar(&b, '\n');
                    break;
                default:
                    return -1;
            }
        }

        else
            luaL_addchar(&b, c);
    }

    luaL_pushresult(&b);
    return pos + 1;
}

static int scan_number(lua_State *L, const char *str, int pos)
{
    char c;
    double number = 0;

    bool is_nn = false;
    if(str[pos] == '-')
    {
        is_nn = true;
        ++pos;
    }

    for(; (c = str[pos]) != '\0'; ++pos)
    {
        if(c >= '0' && c <= '9')
        {
            if(number > 0)
                number *= 10;
            number += c - '0';
        }
        else
            break;
    }

    if(c == '.')
    {
        // TODO: fix that
        ++pos;
        for(; (c = str[pos]) != '\0'; ++pos)
        {
            if(c >= '0' && c <= '9')
                ;
            else
                break;
        }
    }

    if(is_nn)
        number = 0 - number;

    lua_pushnumber(L, number);

    return pos;
}

static int scan_json(lua_State *L, const char *str, int pos);

static int scan_object(lua_State *L, const char *str, int pos)
{
    do
    {
        pos = skip_sp(str, pos);
        char c = str[pos];

        if(c == '"')
        {
            ;
        }
        else if(c == ',')
        {
            ++pos;
            continue;
        }
        else if(c == '}')
        {
            ++pos;
            return pos;
        }
        else
        {
            if(c == '/')
            {
                if(str[pos + 1] == '*')
                {
                    pos = skip_comment(str, pos + 2);
                    continue;
                }
            }

            return -1;
        }

        // key
        pos = scan_string(L, str, pos + 1);
        if(pos == -1)
            return -1;

        pos = skip_sp(str, pos);
        if(str[pos] != ':')
        {
            lua_pop(L, 1);
            return -1;
        }

        // value
        pos = skip_sp(str, pos + 1);
        pos = scan_json(L, str, pos);
        if(pos == -1)
        {
            lua_pop(L, 1);
            return -1;
        }

        lua_settable(L, -3);
    } while(true);
}

static int scan_array(lua_State *L, const char *str, int pos)
{
    int key = 1;
    do
    {
        pos = skip_sp(str, pos);
        char c = str[pos];

        if(c == ',')
        {
            ++pos;
            continue;
        }
        else if(c == ']')
        {
            ++pos;
            return pos;
        }
        else
        {
            if(c == '/')
            {
                if(str[pos + 1] == '*')
                {
                    pos = skip_comment(str, pos + 2);
                    continue;
                }
            }
        }

        lua_pushnumber(L, key);

        pos = scan_json(L, str, pos);
        if(pos == -1)
        {
            lua_pop(L, 1);
            return -1;
        }

        lua_settable(L, -3);
        ++key;
    } while(true);
}

static int scan_json(lua_State *L, const char *str, int pos)
{
    pos = skip_sp(str, pos);
    char c = str[pos];

    switch(c)
    {
        case '\0':
            return -1;

        case '{':
        {
            lua_newtable(L);
            pos = scan_object(L, str, pos + 1);
            if(pos == -1)
                lua_pop(L, 1);
            break;
        }

        case '[':
        {
            lua_newtable(L);
            pos = scan_array(L, str, pos + 1);
            if(pos == -1)
                lua_pop(L, 1);
            break;
        }

        case '/':
        {
            if(str[pos + 1] == '*')
                pos = skip_comment(str, pos + 2);
            else
                pos = -1;
            break;
        }

        case '"':
        {
            pos = scan_string(L, str, pos + 1);
            break;
        }

        default:
        {
            if((c >= '0' && c <= '9') || (c == '-') || (c == '.'))
            { // scan number
                pos = scan_number(L, str, pos);
            }
            else if(!strncmp(&str[pos], "true", 4))
            {
                lua_pushboolean(L, true);
                pos = pos + 4;
            }
            else if(!strncmp(&str[pos], "false", 5))
            {
                lua_pushboolean(L, false);
                pos = pos + 5;
            }
            else if(!strncmp(&str[pos], "null", 4))
            {
                lua_pushnil(L);
                pos = pos + 4;
            }
            break;
        }
    }

    return pos;
}

static int json_decode(lua_State *L)
{
    luaL_checktype(L, -1, LUA_TSTRING);
    const int top = lua_gettop(L);
    scan_json(L, lua_tostring(L, -1), 0);
    if(top == lua_gettop(L))
        lua_pushnil(L);
    return 1;
}

LUA_API int luaopen_json(lua_State *L)
{
    static const luaL_Reg api[] =
    {
        { "encode", json_encode },
        { "decode", json_decode },
        { NULL, NULL }
    };

    luaL_newlib(L, api);
    lua_setglobal(L, "json");

    return 1;
}
