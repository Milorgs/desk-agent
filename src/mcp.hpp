#ifndef DESK_MCP_HPP
#define DESK_MCP_HPP
#include "desk.hpp"
J *mcp_load(App &app);
bool mcp_save(App &app,J *servers);
bool mcp_import(App &app,const char *path);
bool mcp_prepare(App &app,bool explicit_test=false,const char *only=nullptr);
J *mcp_tools(App &app);
void mcp_append_tools(App &app,J *tools);
J *mcp_call(App &app,const char *name,J *arguments);
void mcp_close(App &app);
#endif
