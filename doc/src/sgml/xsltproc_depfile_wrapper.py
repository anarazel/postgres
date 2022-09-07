#!/usr/bin/env python3

import subprocess
import argparse
from unittest import result

parser = argparse.ArgumentParser(
    description='generate dependency file for docs')

parser.add_argument('--targetname', type=str, required=False, nargs='+')
parser.add_argument('--depfile', type=str, required=False)
parser.add_argument('--xsltproc', type=str, required=True)

parser.add_argument('xsltproc_flags', nargs='*')

args = parser.parse_args()

if args.depfile:
    depfile_result_list = []

    command = [args.xsltproc, '--load-trace'] + args.xsltproc_flags
    # --load-trace flag displays all the documents
    # loaded during the processing to stderr
    res = subprocess.run(command, stderr=subprocess.PIPE,
                         universal_newlines=True)
    # if exit code is different than 0, exit
    if res.returncode:
        exit(res.returncode)

    # get line start from targetnames
    line_start = ''
    for name in args.targetname:
        line_start = line_start + name + ' '
    line_start = line_start.strip() + ': '

    # collect only file paths
    with open(args.depfile, 'w') as f:
        for line in res.stderr.split('\n'):
            if line and 'http:' not in line:
                f.write(line_start + line.split('\"')[1] + '\n')
else:
    command = [args.xsltproc] + args.xsltproc_flags
    res = subprocess.run(command)
    exit(res.returncode)
