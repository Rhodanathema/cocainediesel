#include "qcommon/base.h"

bool break1 = false;
bool break2 = false;
bool break3 = false;
bool break4 = false;

void format( FormatBuffer * fb, Span< const char > span, const FormatOpts & opts ) {
	if( fb->capacity > 0 && fb->len < fb->capacity - 1 ) {
		size_t len = Min2( span.n, fb->capacity - fb->len - 1 );
		memcpy( fb->buf + fb->len, span.ptr, len );
		fb->buf[ fb->len + len ] = '\0';
	}
	fb->len += span.n;
}

char * CopyString( Allocator * a, const char * str ) {
	char * copy = ALLOC_MANY( a, char, strlen( str ) + 1 );
	strcpy( copy, str );
	return copy;
}

Span< const char > MakeSpan( const char * str ) {
	return Span< const char >( str, strlen( str ) );
}
