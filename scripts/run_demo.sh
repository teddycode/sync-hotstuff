#!/bin/bash
rep=({0..2})
if [[ $# -gt 0 ]]; then
    rep=($@)
fi
for i in "${rep[@]}"; do
    echo "starting replica $i"
    #valgrind --leak-check=full ./examples/hotstuff-app --conf hotstuff-sec${i}.conf > log${i} 2>&1 &
    #gdb -ex r -ex bt -ex q --args ./examples/hotstuff-app --conf hotstuff-sec${i}.conf > log${i} 2>&1 &
    ./bin/hotstuff-app --conf ./configs/hotstuff-sec${i}.conf > log${i} 2>&1 &
done
wait
