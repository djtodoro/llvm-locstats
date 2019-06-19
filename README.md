# The llvm-locstats

This tool calculates and reports verbose output for the debug location coverage of a binary.

It is very similar to a tool (locstats) from the Elfutils package, but the tool is not
being maintained and released for a long time.

The llvm-locstats for each variable or formal parameter DIE computes what percentage from the code section bytes, where it is in scope, it has location description. There are options to ignore inlined instances or/and entry value locations.
The line *0* shows the number (and the percentage) of DIEs with no location information, but the line *100* shows the number (and the percentage) of DIEs where there is location information in all code section bytes (where the variable or parameter is in the scope). The line *51..59* shows the number (and the percentage) of DIEs where the location information is in between *51* and *59* percentage of its scope covered.

## Building the tool

The build guidelines: https://llvm.org/docs/CMake.html

## Using the tool

1. Running the tool on a simple test case:

 *bin/llvm-locstats test*

    =================================================
              Debug Location Statistics
    =================================================
      cov%          samples       percentage
    -------------------------------------------------
        0                1              25%
        1..9             0               0%
        11..19           0               0%
        21..29           0               0%
        31..39           0               0%
        41..49           0               0%
        51..59           0               0%
        61..69           0               0%
        71..79           0               0%
        81..89           0               0%
        91..99           0               0%
        100              3              75%
    =================================================
      -the number of debug variables processed: 4
      -the average coverage per var: ~ 75%
    =================================================

2. Running the tool on the GDB 7.11 binary:

 *bin/llvm-locstats gdb*
 
    =================================================
              Debug Location Statistics
    =================================================
      cov%          samples       percentage
    -------------------------------------------------
        0             8495               8%
        1..9          3037               3%
        11..19        3015               3%
        21..29        2769               2%
        31..39        2802               2%
        41..49        2766               2%
        51..59        3141               3%
        61..69        3173               3%
        71..79        3923               3%
        81..89        4948               5%
        91..99        7354               7%
        100          53433              54%
    =================================================
      -the number of debug variables processed: 98856
      -the average coverage per var: ~ 76%
    =================================================

3. Running the tool on the GDB 7.11 binary by ignoring debug entry values:

 *bin/llvm-locstats gdb*
 
    =================================================
              Debug Location Statistics
    =================================================
      cov%          samples       percentage
    -------------------------------------------------
        0             8765               8%
        1..9          3948               3%
        11..19        4436               4%
        21..29        4152               4%
        31..39        4107               4%
        41..49        3846               3%
        51..59        4490               4%
        61..69        4140               4%
        71..79        4998               5%
        81..89        7072               7%
        91..99       15478              15%
        100          33424              33%
    =================================================
      -the number of debug variables processed: 98856
      -the average coverage per var: ~ 69%
    =================================================
 
