# vCPU pinning (src/vm/vmpin.c, docs/VIRTUALIZATION.md 3.7.1): the choice of host CPUs over invented computers (SMT,
# hybrid P/E, two packages, small hosts, a cpuset, other machines' cores), the sysfs reader over invented trees, and
# setting it on the test's own threads; under ASan and UBSan. `build/vmpin-test --plan N` prints this computer's plan.
test: build/vmpin-test
build/vmpin-test: tests/vmpin_test.c src/vm/vmpin.c src/vm/vmpin.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined $(WARN) -std=gnu11 -o $@ tests/vmpin_test.c src/vm/vmpin.c -lpthread
	./$@ || { rm -f $@; exit 1; }
