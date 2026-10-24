# Made-up spaces for the README's GIF and screenshots: a home, a scratch, a project (T: where, /tmp/bonsai-demo-<user> by default)
set -e
T=${T:-/tmp/bonsai-demo-$(id -un)}; rm -rf "$T"; mkdir -p "$T"
H=$T/home; S=$T/scratch; P=$T/r12345
f() { mkdir -p "$(dirname "$1")"; fallocate -l "$2" "$1"; }
many() { mkdir -p "$1"; for i in $(seq 1 $2); do echo "x" > "$1/$3$i"; done; }
# home
f $H/simulations/run_2025_03/output.h5 700M; f $H/simulations/run_2025_03/restart.chk 200M
f $H/simulations/run_2025_07/output.h5 450M
for i in 1 2 3 4; do f $H/simulations/sweep_alpha/a0$i.h5 80M; done
f $H/code/solver/build/libsolver.so 160M; f $H/code/solver/build/CMakeFiles/solver.dir/main.o 60M
f $H/code/solver/.git/objects/pack/pack-3f2a.pack 90M; many $H/code/solver/src 300 kernel_ ; many $H/code/solver/tests 120 test_
f $H/code/analysis/.venv/lib/python3.11/site-packages/torch/lib/libtorch_cuda.so 260M
f $H/code/analysis/notebooks/.ipynb_checkpoints/explore-checkpoint.ipynb 12M; many $H/code/analysis/notebooks 40 nb_
f $H/.cache/pip/http/v2/wheels.bin 320M; many $H/.cache/pip/http/v2 500 entry_
f $H/.cache/huggingface/hub/models--bert/model.safetensors 400M
f $H/.conda/pkgs/cudatoolkit-12.4.tar.bz2 300M; many $H/.conda/pkgs/info 200 meta_
f $H/datasets/era5/2024.nc 380M; f $H/datasets/mnist/train.bin 60M
f $H/papers/thesis.pdf 9M
# scratch
for i in 0 1 2 3 4 5; do f $S/checkpoints/ckpt_00$i.pt 420M; done
f $S/tmp_runs/job_51234/out.dat 300M; f $S/tmp_runs/job_51302/out.dat 250M; many $S/tmp_runs/job_51234/frames 12000 frame_
f $S/core.48213 520M
for i in 1 2 3; do f $S/old_campaign_2024/results/r$i.h5 400M; done
f $S/slurm-51234.out 1M
# project
for y in 2021 2022 2023 2024; do f $P/shared_data/climate/raw/$y.nc 600M; done
f $P/envs/py311/lib/libcudnn.so.9 900M; many $P/envs/py311/lib/python3.11 600 mod_
f $P/releases/v1.2/pkgs/cache/repodata.json 350M; f $P/releases/v1.3/pkgs/cache/repodata.json 350M
for i in 1 2 3 4; do f $P/results/runs/run$i/field.h5 250M; done
# Dates, for the ages and the cleanup's old data: the folders above keep the oldest one (a folder's age is its newest entry's)
when() { local d=$1; shift; find "$@" -exec touch -d "$d" {} +; }
when "2024-01-15 10:00" $T
when "2024-02-10 12:00" $S/old_campaign_2024
when "2025-03-20 09:00" $H/simulations/run_2025_03 $H/simulations/sweep_alpha
when "2025-09-02 18:00" $H/simulations/run_2025_07
for y in 2021 2022 2023 2024; do when "$((y + 1))-01-04 08:00" $P/shared_data/climate/raw/$y.nc; done
when "430 days ago" $H/datasets/mnist; when "240 days ago" $H/papers; when "200 days ago" $H/datasets/era5
when "150 days ago" $H/.conda; when "95 days ago" $P/releases; when "60 days ago" $P/envs
when "45 days ago" $S/core.48213; when "30 days ago" $P/results; when "18 days ago" $S/tmp_runs $S/slurm-51234.out
when "9 days ago" $H/code/analysis; when "6 days ago" $S/checkpoints; when "3 days ago" $H/code/solver
when "5 hours ago" $H/.cache
echo "demo tree: $(find $T -type f | wc -l) files, $(du -sh $T | cut -f1)"
