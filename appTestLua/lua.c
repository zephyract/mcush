#include "mcush.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#define LUA_MAXINPUT    512
#define LUA_PROMPT      "> "
#define LUA_PROMPT2     ">> "

/*
 * lua_readline defines how to show a prompt and then read a line from the standard input.
 * lua_saveline defines how to "save" a read line in a "history".
 * lua_freeline defines how to free a line read by lua_readline.
 */
#define lua_readline(L,b,p)  ((void)L, shell_read_line(b, p)!=-1)
#define lua_saveline(L,line) { (void)L; (void)line; }
#define lua_freeline(L,b)    { (void)L; (void)b; }


/* print message */
static void lua_message( const char *prompt, const char *msg )
{
    if( prompt )
    {
        shell_write_str( prompt );
        shell_write_str( ": " );
    }    
    shell_write_line( msg );
}

/*
** Message handler used to run all chunks
*/
static int msghandler (lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == NULL) {  /* is error object not a string? */
    if (luaL_callmeta(L, 1, "__tostring") &&  /* does it have a metamethod */
        lua_type(L, -1) == LUA_TSTRING)  /* that produces a string? */
      return 1;  /* that is the message */
    else
      msg = lua_pushfstring(L, "(error object is a %s value)",
                               luaL_typename(L, 1));
  }
  luaL_traceback(L, L, msg, 1);  /* append a standard traceback */
  return 1;  /* return the traceback */
}


/*
** Check whether 'status' is not OK and, if so, prints the error
** message on the top of the stack. It assumes that the error object
** is a string, as it was either generated by Lua or by 'msghandler'.
*/
static int lua_print_error( lua_State *L, int status )
{
  if (status != LUA_OK) {
    const char *msg = lua_tostring(L, -1);
    lua_message( "lua", msg );
    lua_pop(L, 1);  /* remove message */
  }
  return status;
}


/*
** Interface to 'lua_pcall', which sets appropriate message function
** and C-signal handler. Used to run all chunks.
*/

static int docall (lua_State *L, int narg, int nres) {
  int status;
  int base = lua_gettop(L) - narg;  /* function index */
  lua_pushcfunction(L, msghandler);  /* push message handler */
  lua_insert(L, base);  /* put it under function and args */
#if 0
  globalL = L;  /* to be available to 'laction' */
  signal(SIGINT, laction);  /* set C-signal handler */
#endif
  status = lua_pcall(L, narg, nres, base);
#if 0
  signal(SIGINT, SIG_DFL); /* reset C-signal handler */
#endif
  lua_remove(L, base);  /* remove message handler from the stack */
  return status;
}

//static int dochunk (lua_State *L, int status) {
//  if (status == LUA_OK) status = docall(L, 0, 0);
//  return report(L, status);
//}
//
//static int dofile (lua_State *L, const char *name) {
//  return dochunk(L, luaL_loadfile(L, name));
//}
//
//static int dostring (lua_State *L, const char *s, const char *name) {
//  return dochunk(L, luaL_loadbuffer(L, s, strlen(s), name));
//}

/*
** Returns the string to be used as a prompt by the interpreter.
*/
static const char *get_prompt (lua_State *L, int firstline) {
    const char *p;
    lua_getglobal(L, firstline ? "_PROMPT" : "_PROMPT2");
    p = lua_tostring(L, -1);
    if( p == NULL )
    {
        p = (firstline ? LUA_PROMPT : LUA_PROMPT2);
    }
    return p;
}

/* mark in error messages for incomplete statements */
#define EOFMARK		"<eof>"
#define marklen		(sizeof(EOFMARK)/sizeof(char) - 1)


/*
** Check whether 'status' signals a syntax error and the error
** message at the top of the stack ends with the above mark for
** incomplete statements.
*/
static int incomplete (lua_State *L, int status) {
  if (status == LUA_ERRSYNTAX) {
    size_t lmsg;
    const char *msg = lua_tolstring(L, -1, &lmsg);
    if (lmsg >= marklen && strcmp(msg + lmsg - marklen, EOFMARK) == 0) {
      lua_pop(L, 1);
      return 1;
    }
  }
  return 0;  /* else... */
}


/*
** Prompt the user, read a line, and push it into the Lua stack.
*/
static int pushline (lua_State *L, int firstline) {
  char buffer[LUA_MAXINPUT];
  char *b = buffer;
  size_t l;
  const char *prmt = get_prompt(L, firstline);
  int readstatus = lua_readline(L, b, prmt);
  //shell_printf("len(%d) %s\n", strlen(b), b);
  if (readstatus == 0)
    return 0;  /* no input (prompt will be popped by caller) */
  lua_pop(L, 1);  /* remove prompt */
  l = strlen(b);
  if (l > 0 && b[l-1] == '\n')  /* line ends with newline? */
    b[--l] = '\0';  /* remove it */
  if (firstline && b[0] == '=')  /* for compatibility with 5.2, ... */
    lua_pushfstring(L, "return %s", b + 1);  /* change '=' to 'return' */
  else
    lua_pushlstring(L, b, l);
  lua_freeline(L, b);
  return 1;
}


/*
** Try to compile line on the stack as 'return <line>'; on return, stack
** has either compiled chunk or original line (if compilation failed).
*/
static int addreturn (lua_State *L) {
  int status;
  size_t len; const char *line;
  lua_pushliteral(L, "return ");
  lua_pushvalue(L, -2);  /* duplicate line */
  lua_concat(L, 2);  /* new line is "return ..." */
  line = lua_tolstring(L, -1, &len);
  if ((status = luaL_loadbuffer(L, line, len, "=stdin")) == LUA_OK) {
    lua_remove(L, -3);  /* remove original line */
    line += sizeof("return")/sizeof(char);  /* remove 'return' for history */
    if (line[0] != '\0')  /* non empty? */
      lua_saveline(L, line);  /* keep history */
  }
  else
    lua_pop(L, 2);  /* remove result from 'luaL_loadbuffer' and new line */
  return status;
}


/*
** Read multiple lines until a complete Lua statement
*/
static int multiline (lua_State *L) {
  for (;;) {  /* repeat until gets a complete statement */
    size_t len;
    const char *line = lua_tolstring(L, 1, &len);  /* get what it has */
    int status = luaL_loadbuffer(L, line, len, "=stdin");  /* try it */
    if (!incomplete(L, status) || !pushline(L, 0)) {
      lua_saveline(L, line);  /* keep history */
      return status;  /* cannot or should not try to add continuation line */
    }
    lua_pushliteral(L, "\n");  /* add newline... */
    lua_insert(L, -2);  /* ...between the two lines */
    lua_concat(L, 3);  /* join them */
  }
}


/*
** Read a line and try to load (compile) it first as an expression (by
** adding "return " in front of it) and second as a statement. Return
** the final status of load/call with the resulting function (if any)
** in the top of the stack.
*/
static int loadline (lua_State *L) {
  int status;
  lua_settop(L, 0);
  if (!pushline(L, 1))
    return -1;  /* no input */
  if ((status = addreturn(L)) != LUA_OK)  /* 'return ...' did not work? */
    status = multiline(L);  /* try as command, maybe with continuation lines */
  lua_remove(L, 1);  /* remove line from the stack */
  lua_assert(lua_gettop(L) == 1);
  return status;
}


/*
** Prints (calling the Lua 'print' function) any values on the stack
*/
static void lua_print( lua_State *L )
{
    int n = lua_gettop(L);

    if( n > 0 )
    {
        luaL_checkstack(L, LUA_MINSTACK, "too many results to print");
        lua_getglobal(L, "print");
        lua_insert(L, 1);
        if( lua_pcall(L, n, 0, 0) != LUA_OK )
            lua_message( "lua", lua_pushfstring(L, "error calling 'print' (%s)",
                                               lua_tostring(L, -1)));
    }
}

/* REPL: repeatedly read (load) a line, evaluate (call) it, and print results.
 */
static void lua_repl( lua_State *L )
{
    int status;

    while( (status = loadline(L)) != -1 )
    {
        if( status == LUA_OK )
            status = docall(L, 0, LUA_MULTRET);
        if( status == LUA_OK )
            lua_print(L);
        else
            lua_print_error(L, status);
    }
    lua_settop(L, 0);  /* clear stack */
    lua_writeline();
}


extern int luaopen_ledlib(lua_State *L);
extern int luaopen_gpiolib(lua_State *L);
extern int luaopen_loglib(lua_State *L);

int cmd_lua( int argc, char *argv[] )
{
    lua_State *L = luaL_newstate();

    if( L == NULL )
    {
        shell_write_err(shell_str_memory);
        return -1;
    }

    luaL_openlibs(L);  /* open standard libs */

    luaopen_package(L);
   
    /* append led lib */ 
    luaL_requiref(L, "led", luaopen_ledlib, 1 );
    lua_pop(L, 1);
    luaL_requiref(L, "gpio", luaopen_gpiolib, 1 );
    lua_pop(L, 1);
    luaL_requiref(L, "log", luaopen_loglib, 1 );
    lua_pop(L, 1);


#if 0
    //dostring(L,"print('hello')","Test_lua");
    //dofile(L, NULL);  /* executes stdin as a file */
#endif
    lua_repl(L);
    
    lua_close(L);
    return 0;
}
