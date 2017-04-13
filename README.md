# zoomascii
Faster versions of Python ASCII functions, in the theme of binascii
with added zoom.  These functions only work on ASCII strings and will
trade memory for speed (within reason).  Currently targeting Python
2.7.x, but patches welcome for Python 3.x.

Currently implemented:

swapcase - over 10x faster than Python's builtin swapcase() for ASCII
strings.  (I don't expect this is actually useful, just did it as a
proof-of-concept.)

b2a_qp - hot code for email sending apps that use QP encoding.

If you'd like to see others, let me know!
