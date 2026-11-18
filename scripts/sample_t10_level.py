#!/usr/bin/env python3
"""Sample gpio569 (EMIO 57 = T10 = J3 Pin 6) value in a tight loop to count transitions."""
import time, sys

dpath = '/sys/class/gpio/gpio569/direction'
vpath = '/sys/class/gpio/gpio569/value'

f = open(dpath, 'w'); f.write('in'); f.close()
print('direction set to "in"')

last = None
changes = 0
ones = 0
zeros = 0
t0 = time.time()
while time.time() - t0 < 0.5:
    v = open(vpath).read().strip()
    if v != last:
        changes += 1
        last = v
    if v == '1':
        ones += 1
    else:
        zeros += 1
print(f'0.5s sample: changes={changes} ones={ones} zeros={zeros} last={last}')
