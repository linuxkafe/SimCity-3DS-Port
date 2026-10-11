type: MAJOR
title: Delivered binary reports Tag_CPU_arch v7 / "7-A" while target and all linked libs are armv6k; CMake forbids -march=armv7
personas: cinico (sole reporter of this form; see SYNTHESIS note)
evidence: 'readelf -A build/simcity.3dsx | grep Tag_CPU_arch' -> v7, Tag_CPU_name "7-A". CMakeLists.txt:166,173 mandate -march=armv6k -mtune=mpcore; 278-280 link only armv6k/fpu libgcc/libc/libm. ls 3dsx_*.ld -> 3 abandoned arch-patch experiments. T012-review.md:66 asserts reproducibility from toolchain INPUTS, never inspects artifact architecture.
closure-condition: 'readelf -A build/simcity.3dsx | grep Tag_CPU_arch:' reports v6K AND 'ls 3dsx_*.ld 2>/dev/null | wc -l' returns 0.
rubric-criterion: E-04
