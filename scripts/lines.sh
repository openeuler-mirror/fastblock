#!/bin/bash

RootPath=`git rev-parse --show-toplevel`
find $RootPath -name "*.cc"|xargs cat|wc -l

