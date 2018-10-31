#!/bin/bash

# Script to do the custom build (slic3r and svgviewer)
# example:
# slic3r-last --dont-arrange -m cyl1.STL cyl2.STL --export-svg --output
#             ./svgviewer/model.svg
# open ./svgviewer/svgviewer.html -a firefox

### Constants
export_file=model.svg
config_file=config/config.ini
view_script=./tests/svgviewer/svgviewer.html
log_file=out.txt
FILES=


# ----------- Sanity Check ----------------------
# if the nr of args(#) is not (numerically) equal to 1, output usage to stderr
# and exit with a failure status code
# src: https://stackoverflow.com/questions/4341630/checking-for-the-correct-number-of-arguments
if [ "$#" -lt 2 ]; then
  echo "Usage: $0 input_file1 input_file2 ... input_fileN" >&2
  exit 1
fi
# ---------------------------------------------------

## Loop until all filenames are captured
## src: http://linuxcommand.org/lc3_wss0120.php
while [ "$1" != "" ]; do
    # concat src: https://stackoverflow.com/a/2250199
    FILES="${FILES}$1 "
    # Shift all the parameters down by one
    shift
done


# Remove export file if it exists
if [ -e $export_file ]
then
    rm $export_file
fi

# Build the command before running it
run=('time ./slic3r --load $config_file --export-svg $FILES --output $export_file > $log_file')

# Run cmd
if eval $run; then
    cp $export_file tests/svgviewer/
    echo "<!-- " >> $export_file
    echo "................ Config options ................." >> $export_file
    cat $config_file >> $export_file 
    echo "................................................." >> $export_file
    echo "-->" >> $export_file
    open "$view_script" -a firefox
else
    echo "Slicing failed"
fi

################ MAIN (for Slic3r.pl - with merge) ###########################
## Loop until all filenames are captured
## src: http://linuxcommand.org/lc3_wss0120.php
#while [ "$1" != "" ]; do
#    # concat src: https://stackoverflow.com/a/2250199
#    FILES="${FILES}$1 "
#    # Shift all the parameters down by one
#    shift
#done
#
#"$slic" --dont-arrange -m "cyl1.STL cyl2.STL" --export-svg --output "$out_file"
# conditional exec src: https://unix.stackexchange.com/a/22728
#if "$slic" --dont-arrange -m "$FILES" --export-svg --output "$out_file"; then
#  open "$view_script" -a firefox
#else
#  echo "Slicing failed"
#fi
#############################################################################
