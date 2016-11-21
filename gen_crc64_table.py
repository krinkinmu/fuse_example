#!/usr/bin/python

def gen_table(poly):
    table = [[]]
    for b in xrange(256):
        crc = b
        for _ in xrange(8):
            crc = poly ^ (crc >> 1) if crc & 1 else crc >> 1
        table[-1].append(crc)
    for i in xrange(1, 8):
        table.append([])
        for pos in xrange(256):
            x = table[i - 1][pos]
            table[i].append((x >> 8) ^ table[0][x & 0xff])
    return table

def print_table(table, name = "crc_table", cols = 2):
    tables, entries = len(table), len(table[0])
    last = (entries + cols - 1) / cols - 1
    print "static const unsigned long long {}[{}][{}] = {{".format(name,
        tables, entries)
    for tbl in table:
        print "\t{"
        for row in xrange((entries + cols - 1) / cols):
            begin, end = row * cols, (row + 1) * cols
            line = ", ".join(map("0x{:016x}ull".format, tbl[begin: end]))
            print "\t\t" + line + ["", ","][row != last]
        print "\t},"
    print "};"

#polynomial = 0xc96c5795d7870f42 # adler
polynomial = 0x95ac9329ac4bc9b5 # "Jones"

print "/* This file is generated automatically, don't change it. */"
print "/* Reversed polynomial: 0x{:016x} */".format(polynomial)
print_table(gen_table(polynomial))
