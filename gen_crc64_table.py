#!/usr/bin/python

def gen_table(poly):
    table = []
    for b in xrange(256):
        crc = b
        for _ in xrange(8):
            crc = poly ^ (crc >> 1) if crc & 1 else crc >> 1
        table.append(crc)
    return table

def print_table(table, name = "crc_table", cols = 2):
    last = (len(table) + cols - 1) / cols - 1
    print "static const unsigned long long {}[] = {{".format(name)
    for row in xrange((len(table) + cols - 1) / cols):
        begin, end = row * cols, (row + 1) * cols
        line = ", ".join(map("0x{:016x}ull".format, table[begin: end]))
        print "\t" + line + ["", ","][row != last]
    print "};"

#polynomial = 0xc96c5795d7870f42 # adler
polynomial = 0x95ac9329ac4bc9b5 # "Jones"

print "/* This file is generated automatically, don't change it. */"
print "/* Reversed polynomial: 0x{:016x} */".format(polynomial)
print_table(gen_table(polynomial))
