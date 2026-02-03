#!/bin/bash

mv "$1" .tmp.file
mv "$2" "$1"
mv .tmp.file "$2"
