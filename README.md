# zoomascii

Faster versions of Python ASCII functions, in the theme of binascii
with added zoom.  These functions only work on ASCII strings and will
trade memory for speed (within reason).  Currently targeting Python
2.7.x, but patches welcome for Python 3.x.

## Currently Implemented

b2a_qp - Hot code for email sending apps that use QP encoding, and the
reason I started this library.  Around 3x faster than binascii.b2a_qp.

swapcase - over 10x faster than Python's builtin swapcase() for ASCII
strings.  (I don't expect this is actually useful, just did it as a
proof-of-concept.)

## Install

Get it with pip:

        $ pip install zoomascii

Or clone it from github and install manually:

        $ git clone https://github.com/samtregar/zoomascii.git
        $ cd zoomascii
        $ sudo python setup.py install

## Example Code

        import zoomascii

        # encode data as QP, zoom style
        encoded_data = zoomascii.b2a_qp(text_data)

        # optionally turn off encoding leading periods, which is nice
        # for SMTP but probably your lib already handles this
        encoded_data = zoomascii.b2a_qp(text_data, encode_leading_dot=False)

        # swapcase so fast
        text = zoomascii.swapcase(text)

## Benchmarks

![Benchark Chart](http://i.imgur.com/QBV9z7h.png)

You can find the benchmark script in bin/bench.py.  It runs each
encoder across a series of sample files in data/ and the results are
runs per second.  The total input size for each run is 472k.  The
quopri module is being forced to use its Python implementation rather
than binascii which it will use if installed.

## Implementation Notes

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
