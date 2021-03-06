#!/usr/bin/env python2
# -*- coding: utf-8 -*-
# $Id$
#
# Echo server listening at port 65000, with the following non-standard
# features:
# - any 'x' in the received string is replaced by 'ThisIsLong'
# - 'COMPRESS' in the received string causes the connection to be switched
#   to deflate compression
# - 'DISCONNECT' in the received string causes the connection to be closed
# 
# Copyright (C) Albrecht Dreß <mailto:albrecht.dress@arcor.de> 2017 - 2020
#
# This script is free software; you can redistribute it and/or modify it under
# the terms of the GNU Lesser General Public License as published by the Free
# Software Foundation; either version 2 of the License, or (at your option)
# any later version.
#
# This script is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
# FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
# for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this script. If not, see <http://www.gnu.org/licenses/>.

import sys
import socket
import zlib

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
server_address = ('localhost', 65000)
sock.bind(server_address)
sock.listen(1)

while True:
    connection, client_address = sock.accept()
    try:
        comp = False
        comp_ctx = zlib.compressobj(-1, zlib.DEFLATED, -15)
        decomp_ctx = zlib.decompressobj(-15)
        while True:
            if comp:
                data = decomp_ctx.decompress(connection.recv(2048))
            else:
                data = connection.recv(2048)
            if data:
                print "received: {}".format(data.strip())
                if 'DISCONNECT' in data:
                    break
                data = data.replace('x', 'ThisIsLong')
                if comp:
                    connection.sendall(comp_ctx.compress(data) + comp_ctx.flush(zlib.Z_FULL_FLUSH))
                else:
                    connection.sendall(data)
                if data.startswith('COMPRESS'):
                    print "compression enabled"
                    comp = True
            else:
                break
    finally:
        connection.close()
