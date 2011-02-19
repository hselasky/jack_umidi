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

.if exists(%%PREFIX%%)
PREFIX=		%%PREFIX%%
.else
PREFIX=		/usr/local
.endif

BINDIR=		${PREFIX}/sbin
MANDIR=		${PREFIX}/man/man
LIBDIR?=	${PREFIX}/lib
MKLINT=		no
NOGCCERROR=
MLINKS=
PROG=		jack_umidi
MAN=		jack_umidi.1
SRCS=		jack_umidi.c
CFLAGS+=	-I${PREFIX}/include -Wall

.if defined(HAVE_DEBUG)
CFLAGS+=	-DHAVE_DEBUG
CFLAGS+=	-g
.endif

LDFLAGS+=	-L${LIBDIR} -lpthread -ljack

.include <bsd.prog.mk>
