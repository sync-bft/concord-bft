#!/bin/bash

set -e

gnome-terminal -- ./server -id 0 -cf ../../../tests/simpleTest/scripts/remote_config.txt
gnome-terminal -- ./server -id 1 -cf ../../../tests/simpleTest/scripts/remote_config.txt
gnome-terminal -- ./server -id 2 -cf ../../../tests/simpleTest/scripts/remote_config.txt

gnome-terminal -- ./client -id 3 -cf ../../../tests/simpleTest/scripts/remote_config.txt
gnome-terminal -- ./client -id 4 -cf ../../../tests/simpleTest/scripts/remote_config.txt
gnome-terminal -- ./client -id 5 -cf ../../../tests/simpleTest/scripts/remote_config.txt

# gnome-terminal -- ./client -id 6 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 7 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 8 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 9 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 10 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 11 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 12 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 13 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 14 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 15 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 16 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 17 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 18 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 19 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 20 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 21 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 22 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 23 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 24 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 25 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 26 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 27 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 28 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 29 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 30 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 31 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 32 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 33 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 34 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 35 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 36 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 37 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 38 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 39 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 40 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 41 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 42 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 43 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 44 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 45 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 46 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 47 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 48 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 49 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 50 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 51 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 52 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 53 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 54 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 55 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 56 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 57 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 58 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 59 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 60 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 61 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 62 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 63 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 64 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 65 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 66 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 67 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 68 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 69 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 70 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 71 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 72 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 73 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 74 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 75 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 76 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 77 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 78 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 79 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 80 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 81 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 82 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 83 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 84 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 85 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 86 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 87 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 88 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 89 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 90 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 91 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 92 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 93 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 94 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 95 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 96 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 97 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 98 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 99 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 100 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 101 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 102 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 103 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 104 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 105 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 106 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 107 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 108 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 109 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 110 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 111 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 112 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 113 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 114 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 115 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 116 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 117 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 118 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 119 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 120 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 121 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 122 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 123 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 124 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 125 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 126 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 127 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 128 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 129 -cf ../../../tests/simpleTest/scripts/remote_config.txt
# gnome-terminal -- ./client -id 130 -cf ../../../tests/simpleTest/scripts/remote_config.txt

