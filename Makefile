#
# $FreeBSD: $
#
# Copyright (c) 2011 Hans Petter Selasky. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
# ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
# OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
# HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
# OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
# SUCH DAMAGE.
#
#
# Jack client Makefile for USB/RAW MIDI sockets and character devices.
#

VERSION=1.0.6

PROG=		jack_umidi

PREFIX?=	/usr/local
LOCALBASE?=	/usr/local
BINDIR=		${PREFIX}/sbin
MANDIR=		${PREFIX}/man/man
LIBDIR=		${PREFIX}/lib
INCLUDEDIR=	${PREFIX}/include
MKLINT=		no
NOGCCERROR=
NO_PROFILE=
PTHREAD_LIBS?=  -lpthread

CFLAGS+=	-I${PREFIX}/include -Wall

LDFLAGS+=	-L${LIBDIR} ${PTHREAD_LIBS} -ljack

.if defined(HAVE_DEBUG)
CFLAGS+=	-DHAVE_DEBUG
CFLAGS+=	-g
.endif

SRCS=		jack_umidi.c

.if defined(HAVE_MAN)
MAN=		jack_umidi.8
.else
MAN=
.endif

package:

	make clean cleandepend HAVE_MAN=YES

	tar -cvf temp.tar \
		Makefile jack_umidi.c jack_umidi.8

	rm -rf jack_umidi-${VERSION}

	mkdir jack_umidi-${VERSION}

	tar -xvf temp.tar -C jack_umidi-${VERSION}

	rm -rf temp.tar

	tar --uid=0 --gid=0 -jcvf jack_umidi-${VERSION}.tar.bz2 jack_umidi-${VERSION}

.include <bsd.prog.mk>
