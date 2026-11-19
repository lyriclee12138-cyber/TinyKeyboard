#!/usr/bin/env python3
# -*- coding: ascii -*-
# TinyKeyboard base64 decoder. Only Python built-ins: base64, sys.
#
#   python decode.py b64.txt     ->  writes decoded.txt (UTF-8)
#
# The input file is the base64 text typed by the web page and saved
# from Notepad. BOM, line breaks and spaces are ignored automatically.

import base64, sys
s = open(sys.argv[1], "rb").read()
d = base64.b64decode(s).decode("utf-8")
open("decoded.txt", "wb").write(d.encode("utf-8"))
print("OK: decoded.txt")
