"""
script to stress test zoomascii using random fuzz data using faker -
good for finding segfaults and memory leaks.  Also checks results
against a2b_qp.
"""

import zoomascii
import binascii

from random import randint

from faker import Factory
fake = Factory.create()

total = 100000
count = 0
while(True):
    count += 1

    if randint(1,2) == 1:
        text = fake.binary(length=randint(1,1000000))
    else:
        text = ''
        length = randint(1,50000)
        while len(text) < length:
            text += fake.text()

    qp = zoomascii.b2a_qp(text)
    if (binascii.a2b_qp(qp) != text):
        with open('out.txt', 'wb') as fh:
            fh.write(text)
        raise Exception("Failure found, wrote triggering data to out.txt")

    if count % 100 == 0:
        print "%d of %d tests completed." % (count, total)
    if count == total:
        break
        
