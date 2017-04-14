# zoomascii

Faster versions of Python ASCII functions, in the theme of binascii
with added zoom.  These functions only work on ASCII strings and will
trade memory for speed (within reason).  Currently targeting Python
2.7.x, but patches welcome for Python 3.x.

Currently Implemented
=====================

b2a_qp - Hot code for email sending apps that use QP encoding, and the
reason I started this library.  Around 3x faster than binascii.b2a_qp.

swapcase - over 10x faster than Python's builtin swapcase() for ASCII
strings.  (I don't expect this is actually useful, just did it as a
proof-of-concept.)

Example Code
============

        import zoomascii

        # encode data as QP, zoom style
        encoded_data = zoomascii.b2a_qp(text_data)

        # swapcase so fast
        text = zoomascii.swapcase(text)

Implementation Notes
====================

The implementation of b2a_qp follows the specification for
Quoted-Printable encoding in RFC 2045
(https://www.ietf.org/rfc/rfc2045.txt).  The only exception is that
periods at the beginning of lines are always encoded, which is useful
for SMTP and allowed by the spec.

The implementation does not exactly match binascii's implementation -
in particular it does not attempt to pass through CR or LF characters
that are not part of CRLF pairs.  In my reading of the specification
that is illegal although decoders don't appear to care either way.

No attempt has been made to provide a non-text mode of operation with
respect to CRLF handling.  I can't imagine why anyone would be using
QP encoding with non-text data, but if you do want that then you
shouldn't use this module.
