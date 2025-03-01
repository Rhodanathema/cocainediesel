#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "qcommon/platform.h"
#include "qcommon/types.h"
#include "qcommon/math.h"
#include "gg/ggformat.h"
#include "qcommon/allocators.h"
#include "qcommon/linear_algebra.h"
#include "qcommon/tracy.h"

/*
 * helpers
 */

#define CONCAT_HELPER( a, b ) a##b
#define CONCAT( a, b ) CONCAT_HELPER( a, b )
#define COUNTER_NAME( x ) CONCAT( x, __COUNTER__ )

template< typename To, typename From >
To bit_cast( const From & from ) {
	STATIC_ASSERT( sizeof( To ) == sizeof( From ) );
	To result;
	memcpy( &result, &from, sizeof( result ) );
	return result;
}

#define Fatal( format, ... ) FatalImpl( __FILE__, __LINE__, format, ##__VA_ARGS__ )
#define FatalErrno( msg ) FatalErrnoImpl( msg, __FILE__, __LINE__ )

[[gnu::format( printf, 3, 4 )]] void FatalImpl( const char * file, int line, const char * format, ... );
void FatalErrnoImpl( const char * msg, const char * file, int line );

template< typename T >
constexpr bool HasAnyBit( T haystack, T needle ) {
	return ( haystack & needle ) != 0;
}

template< typename T >
constexpr bool HasAllBits( T haystack, T needle ) {
	return ( haystack & needle ) == needle;
}

/*
 * defer
 */

template< typename F >
struct ScopeExit {
	ScopeExit( F f_ ) : f( f_ ) { }
	~ScopeExit() { f(); }
	F f;
};

struct DeferHelper {
	template< typename F >
	ScopeExit< F > operator+( F f ) { return f; }
};

#define defer [[maybe_unused]] const auto & COUNTER_NAME( DEFER_ ) = DeferHelper() + [&]()

/*
 * Span
 */

Span< char > MakeSpan( char * str );
Span< const char > MakeSpan( const char * str );
void format( FormatBuffer * fb, Span< const char > arr, const FormatOpts & opts );

template< typename T, size_t N >
constexpr Span< T > StaticSpan( T ( &arr )[ N ] ) {
	return Span< T >( arr, N );
}

/*
 * Optional
 */

template< typename T >
Optional< T > MakeOptional( const T & x ) {
	return x;
}

template< typename T >
T Default( const Optional< T > & opt, const T & def ) {
	return opt.exists ? opt.value : def;
}

template< typename T >
bool operator==( const Optional< T > & opt, const T & x ) {
	return opt.exists && opt.value == x;
}

template< typename T >
bool operator==( const T & x, const Optional< T > & opt ) {
	return opt == x;
}

template< typename T >
bool operator==( const Optional< T > & a, const Optional< T > & b ) {
	return ( !a.exists && !b.exists ) || ( a.exists && b.exists && a.value == b.value );
}

template< typename T >
bool operator!=( const Optional< T > & a, const Optional< T > & b ) {
	return !( a == b );
}

/*
 * debug stuff
 */

extern bool break1;
extern bool break2;
extern bool break3;
extern bool break4;

/*
 * Com_Print
 */

[[gnu::format( printf, 1, 2 )]] void Com_Printf( const char *format, ... );
[[gnu::format( printf, 1, 2 )]] void Com_Error( const char *format, ... );

template< typename... Rest >
void Com_GGPrintNL( const char * fmt, const Rest & ... rest ) {
	char buf[ 4096 ];
	ggformat( buf, sizeof( buf ), fmt, rest... );
	Com_Printf( "%s", buf );
}

#define Com_GGPrint( fmt, ... ) Com_GGPrintNL( fmt "\n", ##__VA_ARGS__ )

template< typename... Rest >
void Com_GGError( const char * fmt, const Rest & ... rest ) {
	char buf[ 4096 ];
	ggformat( buf, sizeof( buf ), fmt, rest... );
	Com_Error( "%s", buf );
}
