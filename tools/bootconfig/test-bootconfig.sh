#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only

echo "Boot config test script"

if [ -d "$1" ]; then
  TESTDIR=$1
else
  TESTDIR=.
fi
BOOTCONF=${TESTDIR}/bootconfig
ALIGN=4

INITRD=`mktemp ${TESTDIR}/initrd-XXXX`
TEMPCONF=`mktemp ${TESTDIR}/temp-XXXX.bconf`
OUTFILE=`mktemp ${TESTDIR}/tempout-XXXX`
NG=0

cleanup() {
  rm -f $INITRD $TEMPCONF $OUTFILE
  exit $NG
}

trap cleanup EXIT TERM

NO=1

xpass() { # pass test command
  echo "test case $NO ($*)... "
  if ! ($@ && printf "\t\t[OK]\n"); then
     printf "\t\t[NG]\n"; NG=$((NG + 1))
  fi
  NO=$((NO + 1))
}

xfail() { # fail test command
  echo "test case $NO ($*)... "
  if ! (! $@ && printf "\t\t[OK]\n"); then
     printf "\t\t[NG]\n"; NG=$((NG + 1))
  fi
  NO=$((NO + 1))
}

echo "Basic command test"
xpass $BOOTCONF $INITRD

echo "Delete command should success without bootconfig"
xpass $BOOTCONF -d $INITRD

dd if=/dev/zero of=$INITRD bs=4096 count=1
printf "key = value;" > $TEMPCONF
bconf_size=$(wc -c < $TEMPCONF)
initrd_size=$(wc -c < $INITRD)

echo "Apply command test"
xpass $BOOTCONF -a $TEMPCONF $INITRD
new_size=$(wc -c < $INITRD)

echo "Show command test"
xpass $BOOTCONF $INITRD

echo "File size check"
total_size=$(expr $bconf_size + $initrd_size + 9 + 12 + $ALIGN - 1 )
total_size=$(expr $total_size / $ALIGN)
total_size=$(expr $total_size \* $ALIGN)
xpass test $new_size -eq $total_size

echo "Apply command repeat test"
xpass $BOOTCONF -a $TEMPCONF $INITRD

echo "File size check"
xpass test $new_size -eq $(wc -c < $INITRD)

echo "Delete command check"
xpass $BOOTCONF -d $INITRD

echo "File size check"
new_size=$(wc -c < $INITRD)
xpass test $new_size -eq $initrd_size

echo "No error messge while applying"
dd if=/dev/zero of=$INITRD bs=4096 count=1
printf " \0\0\0 \0\0\0" >> $INITRD
$BOOTCONF -a $TEMPCONF $INITRD > $OUTFILE 2>&1
xfail grep -i "failed" $OUTFILE
xfail grep -i "error" $OUTFILE

echo "Max node number check"

awk '
BEGIN {
  for (i = 0; i < 26; i += 1)
      printf("%c\n", 65 + i % 26)
  for (i = 26; i < 8192; i += 1)
      printf("%c%c%c\n", 65 + i % 26, 65 + (i / 26) % 26, 65 + (i / 26 / 26))
}
' > $TEMPCONF
xpass $BOOTCONF -a $TEMPCONF $INITRD

printf "badnode\n" >> $TEMPCONF
xfail $BOOTCONF -a $TEMPCONF $INITRD

echo "Max key depth check"

printf 'a.b.c.d.e.f.g.h.i.j.k.l.m.n.o.p = "value"\n' > $TEMPCONF
xpass $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF -l $INITRD > $OUTFILE
xpass cmp $TEMPCONF $OUTFILE

printf 'a.b.c.d.e.f.g.h.i.j.k.l.m.n.o.p.q = "value"\n' > $TEMPCONF
xfail $BOOTCONF -a $TEMPCONF $INITRD

echo "Max filesize check"

# Max size is 32767 (including terminal byte)
printf "data = \"" > $TEMPCONF
dd if=/dev/urandom bs=768 count=32 | base64 -w0 >> $TEMPCONF
printf "\"\n" >> $TEMPCONF
xfail $BOOTCONF -a $TEMPCONF $INITRD

dd if=$TEMPCONF of=$OUTFILE bs=1 count=32764
cp $OUTFILE $TEMPCONF
printf "\"\n" >> $TEMPCONF	# add 2 bytes + terminal ('\"\n\0')
xpass $BOOTCONF -a $TEMPCONF $INITRD

echo "Adding same-key values"
cat > $TEMPCONF << EOF
key = bar, baz
key += qux
EOF
echo > $INITRD

xpass $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $OUTFILE
xpass grep -q "bar" $OUTFILE
xpass grep -q "baz" $OUTFILE
xpass grep -q "qux" $OUTFILE

echo "Override same-key values"
cat > $TEMPCONF << EOF
key = bar, baz
key := qux
EOF
echo > $INITRD

xpass $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $OUTFILE
xfail grep -q "bar" $OUTFILE
xfail grep -q "baz" $OUTFILE
xpass grep -q "qux" $OUTFILE

echo "Double/single quotes test"
printf "key = '\"string\"';" > $TEMPCONF
$BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $TEMPCONF
cat $TEMPCONF
xpass grep \'\"string\"\' $TEMPCONF

echo "Repeat same-key tree"
cat > $TEMPCONF << EOF
foo
bar
foo { buz }
EOF
echo > $INITRD

xpass $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $OUTFILE
xpass grep -q "bar" $OUTFILE


echo "Remove/keep tailing spaces"
cat > $TEMPCONF << EOF
foo = val     # comment
bar = "val2 " # comment
EOF
echo > $INITRD

xpass $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $OUTFILE
xfail grep -q 'val[[:space:]]' $OUTFILE
xpass grep -q 'val2[[:space:]]' $OUTFILE

echo "Remove unquoted trailing whitespace at end of file"
printf 'foo = val \t' > $TEMPCONF
$BOOTCONF -l $TEMPCONF > $OUTFILE
printf 'foo = "val"\n' > $TEMPCONF
xpass cmp $TEMPCONF $OUTFILE

echo "Remove trailing whitespace from the last array value at end of file"
printf 'foo = first, last \t' > $TEMPCONF
$BOOTCONF -l $TEMPCONF > $OUTFILE
printf 'foo = "first", "last"\n' > $TEMPCONF
xpass cmp $TEMPCONF $OUTFILE

echo "Keep quoted trailing whitespace at end of file"
printf 'foo = "val "' > $TEMPCONF
$BOOTCONF -l $TEMPCONF > $OUTFILE
printf 'foo = "val "\n' > $TEMPCONF
xpass cmp $TEMPCONF $OUTFILE

echo "Showing correct line:column of no closing brace"
cat > $TEMPCONF << EOF
foo {
bar {
}
EOF
$BOOTCONF -a $TEMPCONF $INITRD 2> $OUTFILE
xpass grep -q "1:1" $OUTFILE

echo "Intermediate null character test"
printf "key = value\n\0extra = data\n" > $TEMPCONF
xfail $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF -a $TEMPCONF $INITRD 2> $OUTFILE
xpass grep -q "Unexpected" $OUTFILE

echo "Trailing null character test"
printf "key = value\n\0" > $TEMPCONF
xpass $BOOTCONF -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $OUTFILE
xpass grep -q "value" $OUTFILE

echo "=== expected failure cases ==="
for i in samples/bad-* ; do
  xfail $BOOTCONF -a $i $INITRD
done

echo "=== expected success cases ==="
for i in samples/good-* ; do
  xpass $BOOTCONF -a $i $INITRD
  x="samples/exp-"`basename $i`
  $BOOTCONF $i > $TEMPCONF
  xpass diff $x $TEMPCONF
done

echo "=== multiple bootconfig tests ==="
dd if=/dev/zero of=$INITRD bs=4097 count=1 2>/dev/null
orig_size=$(wc -c < $INITRD)

printf 'kernel.root = /dev/sda1\nfoo = a\n' > $TEMPCONF
echo "ID validation tests"
xfail $BOOTCONF -i "0bad" -a $TEMPCONF $INITRD
xfail $BOOTCONF -i "9" -a $TEMPCONF $INITRD
xfail $BOOTCONF -i "a,b" -a $TEMPCONF $INITRD
xfail $BOOTCONF -i "+a" -a $TEMPCONF $INITRD
xfail $BOOTCONF -i "bad id" -a $TEMPCONF $INITRD
xfail $BOOTCONF -i "12345678901234567890123456789012" -a $TEMPCONF $INITRD
xfail $BOOTCONF -i "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" -a $TEMPCONF $INITRD
xpass $BOOTCONF -i "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" -a $TEMPCONF $INITRD
xpass $BOOTCONF -d $INITRD
xpass test $orig_size -eq $(wc -c < $INITRD)

echo "Apply named and default bootconfigs"
printf 'kernel.loglevel = 5\nfoo = base\nbar = one\n' > $TEMPCONF
xpass $BOOTCONF -i _alpha -a $TEMPCONF $INITRD
printf 'kernel.loglevel := 7\nfoo := debug\nbar += two\n' > $TEMPCONF
xpass $BOOTCONF -i beta-1.0 -a $TEMPCONF $INITRD
printf 'kernel.quiet\nfoo = def\nbar = zero\n' > $TEMPCONF
xpass $BOOTCONF -a $TEMPCONF $INITRD

$BOOTCONF $INITRD > $OUTFILE
xpass grep -q '\[0\][[:space:]](default)' $OUTFILE
xpass grep -q '\[1\][[:space:]]#BOOTCONFIG-ID:[[:space:]]_alpha' $OUTFILE
xpass grep -q '\[2\][[:space:]]#BOOTCONFIG-ID:[[:space:]]beta-1.0' $OUTFILE

echo "Replace existing named bootconfig in place"
printf 'kernel.loglevel = 4\nfoo = alpha_v2\nbar = one\n' > $TEMPCONF
xpass $BOOTCONF -i _alpha -a $TEMPCONF $INITRD
$BOOTCONF $INITRD > $OUTFILE
xpass grep -q 'alpha_v2' $OUTFILE
xpass grep -q '\[1\][[:space:]]#BOOTCONFIG-ID:[[:space:]]_alpha' $OUTFILE
xpass grep -q '\[2\][[:space:]]#BOOTCONFIG-ID:[[:space:]]beta-1.0' $OUTFILE

echo "Select and merge by ID, index, and +append"
$BOOTCONF -l -i _alpha,beta-1.0 $INITRD > $OUTFILE
xpass grep -q 'foo[[:space:]]=[[:space:]]"debug"' $OUTFILE
xpass grep -q 'bar[[:space:]]=[[:space:]]"one",[[:space:]]"two"' $OUTFILE
xpass grep -q 'kernel.loglevel[[:space:]]=[[:space:]]"7"' $OUTFILE

$BOOTCONF -l -i 1,2 $INITRD > $TEMPCONF
xpass cmp $OUTFILE $TEMPCONF

$BOOTCONF -C -i _alpha,beta-1.0 $INITRD > $OUTFILE
xpass grep -q 'loglevel=7' $OUTFILE

$BOOTCONF -l -i ,beta-1.0 $INITRD > $OUTFILE
xpass grep -q 'kernel.quiet[[:space:]]=[[:space:]]""' $OUTFILE
xpass grep -q 'foo[[:space:]]=[[:space:]]"debug"' $OUTFILE
xpass grep -q 'bar[[:space:]]=[[:space:]]"zero",[[:space:]]"two"' $OUTFILE

$BOOTCONF -l -i +beta-1.0 $INITRD > $TEMPCONF
xpass cmp $OUTFILE $TEMPCONF

echo "Missing ID / index error and cross-file redefinition error"
xfail $BOOTCONF -l -i missing $INITRD
xfail $BOOTCONF -l -i 9 $INITRD
xfail $BOOTCONF -l -i ,_alpha $INITRD
$BOOTCONF -l -i ,_alpha $INITRD 2> $OUTFILE || true
xpass grep -q 'Error[[:space:]]in[[:space:]]bootconfig[[:space:]]\[1:_alpha\]' $OUTFILE

echo "Selective delete and delete all"
xfail $BOOTCONF -d -i missing $INITRD
xpass $BOOTCONF -d -i _alpha $INITRD
$BOOTCONF $INITRD > $OUTFILE
xpass grep -q '\[0\][[:space:]](default)' $OUTFILE
xpass grep -q '\[1\][[:space:]]#BOOTCONFIG-ID:[[:space:]]beta-1.0' $OUTFILE
xfail grep -q '_alpha' $OUTFILE

xpass $BOOTCONF -d -i 0 $INITRD
$BOOTCONF $INITRD > $OUTFILE
xpass grep -q '\[0\][[:space:]]#BOOTCONFIG-ID:[[:space:]]beta-1.0' $OUTFILE
xfail grep -q 'quiet' $OUTFILE

xpass $BOOTCONF -d $INITRD
xpass test $orig_size -eq $(wc -c < $INITRD)

echo "Max stacked configs (32) limit check"
i=0
while [ $i -lt 32 ]; do
  printf "k_%d = v_%d\n" $i $i > $TEMPCONF
  if ! $BOOTCONF -i "cfg_$i" -a $TEMPCONF $INITRD > /dev/null; then
    break
  fi
  i=$((i + 1))
done
xpass test $i -eq 32
printf "k_extra = 1\n" > $TEMPCONF
xfail $BOOTCONF -i cfg_32 -a $TEMPCONF $INITRD
$BOOTCONF -l -i 31 $INITRD > $OUTFILE
xpass grep -q 'k_31[[:space:]]=[[:space:]]"v_31"' $OUTFILE
xpass $BOOTCONF -d $INITRD
xpass test $orig_size -eq $(wc -c < $INITRD)

echo "Total stacked size > 32KB with valid per-selection size"
printf "big1 = \"" > $TEMPCONF
dd if=/dev/zero bs=1 count=20000 2>/dev/null | tr '\0' 'a' >> $TEMPCONF
printf "\"\n" >> $TEMPCONF
xpass $BOOTCONF -i big1 -a $TEMPCONF $INITRD
printf "big2 = \"" > $TEMPCONF
dd if=/dev/zero bs=1 count=20000 2>/dev/null | tr '\0' 'b' >> $TEMPCONF
printf "\"\n" >> $TEMPCONF
xpass $BOOTCONF -i big2 -a $TEMPCONF $INITRD
$BOOTCONF -l -i big1 $INITRD > $OUTFILE
xpass grep -q 'big1[[:space:]]=' $OUTFILE
$BOOTCONF -l -i big2 $INITRD > $OUTFILE
xpass grep -q 'big2[[:space:]]=' $OUTFILE
xfail $BOOTCONF -l -i big1,big2 $INITRD
xpass $BOOTCONF -d $INITRD
xpass test $orig_size -eq $(wc -c < $INITRD)


echo
echo "=== Summary ==="
echo "# of Passed: $(expr $NO - $NG - 1)"
echo "# of Failed: $NG"

echo
if [ $NG -eq 0 ]; then
	echo "All tests passed"
else
	echo "$NG tests failed"
	exit 1
fi
