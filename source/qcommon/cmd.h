#pragma once

#include "qcommon/types.h"

void Cmd_Init();
void Cmd_Shutdown();

bool Cmd_ExecuteLine( Span< const char > line, bool warn_on_invalid );
void Cmd_ExecuteLine( const char * line );

template< typename... Rest >
void Cmd_Execute( const char * fmt, const Rest & ... rest ) {
	char buf[ 1024 ];
	ggformat( buf, sizeof( buf ), fmt, rest... );
	Cmd_ExecuteLine( buf );
}

void Cmd_ExecuteEarlyCommands( int argc, char ** argv );
void Cmd_ExecuteLateCommands( int argc, char ** argv );

using ConsoleCommandCallback = void ( * )( const char * args, Span< Span< const char > > tokens );
using TabCompletionCallback = Span< const char * > ( * )( TempAllocator * a, const char * partial );

void AddCommand( const char * name, ConsoleCommandCallback function );
void SetTabCompletionCallback( const char * name, TabCompletionCallback callback );
void RemoveCommand( const char * name );

Span< const char * > TabCompleteCommand( TempAllocator * a, const char * partial );
Span< const char * > SearchCommands( Allocator * a, const char * partial );
Span< const char * > TabCompleteArgument( TempAllocator * a, const char * partial );
Span< const char * > TabCompleteFilename( TempAllocator * a, const char * partial, const char * search_dir, const char * extension );
Span< const char * > TabCompleteFilenameHomeDir( TempAllocator * a, const char * partial, const char * search_dir, const char * extension );

Span< Span< const char > > TokenizeString( Allocator * a, const char * text );

void ExecDefaultCfg();
