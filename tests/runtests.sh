echo "Running unit tests:"

for file in tests/*_tests
do
    [ -x "$file" ] || continue
    $VALGRIND "./$file"
done
