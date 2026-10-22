/*
 * H1 userspace compatibility for iOS 16.x. Included by sweet_amfi.c only.
 * Independent implementation informed by HackiOS (APSL 2.0), ideas only.
 * See docs-lanes/kernel-h1.md for ABI evidence and security implications.
 */
#ifndef FUXI_AMFI_H
#define FUXI_AMFI_H

static TUNABLE(uint32_t, fuxi_amfi_enabled, "fuxiamfi", 1);

#define FUXI_AMFI_DYLD_POLICY       0x5a
#define FUXI_AMFI_BOOTARG_STATE     0x60
#define FUXI_AMFI_DYLD_FEATURES     0xffULL /* all eight iOS 16 dyld features */
#define FUXI_AMFI_NOT_ENFORCING     0x86ULL /* ExtensionKit tests bits 1, 2, 7 */

struct fuxi_dyld_query {
	uint64_t input;
	user_addr_t output;
};

struct fuxi_container_query {
	int64_t pid;
	uint64_t version;
	user_addr_t path;
	user_size_t capacity;
	uint64_t check_version;
};

/* Sparse requests include negative/unknown ints, not just the first 64. */
struct fuxi_amfi_request {
	int number;
	struct fuxi_amfi_request *next;
};
static struct fuxi_amfi_request *fuxi_amfi_requests;
static LCK_GRP_DECLARE(fuxi_amfi_group, "fuxi-amfi");
static LCK_MTX_DECLARE(fuxi_amfi_lock, &fuxi_amfi_group);

static void
fuxi_amfi_log_request(struct proc *p, int call)
{
	lck_mtx_lock(&fuxi_amfi_lock);
	for (struct fuxi_amfi_request *r = fuxi_amfi_requests; r; r = r->next) {
		if (r->number == call) {
			lck_mtx_unlock(&fuxi_amfi_lock);
			return;
		}
	}
	struct fuxi_amfi_request *r = kalloc_type(struct fuxi_amfi_request, Z_WAITOK | Z_ZERO);
	r->number = call;
	r->next = fuxi_amfi_requests;
	fuxi_amfi_requests = r;
	lck_mtx_unlock(&fuxi_amfi_lock);
	char name[32] = {0};
	proc_name(proc_pid(p), name, sizeof(name));
	printf("FUXI AMFI: %s request %#x\n", name, (unsigned)call);
}

static int
fuxi_amfi_syscall(struct proc *p, int call, user_addr_t arg)
{
	fuxi_amfi_log_request(p, call);
	if (!fuxi_amfi_enabled) {
		return ENOPOLICY;
	}
	uint64_t answer;
	switch (call) {
	case FUXI_AMFI_DYLD_POLICY: {
		struct fuxi_dyld_query query;
		int error = copyin(arg, &query, sizeof(query));
		if (error) {
			return error;
		}
		/* Intentionally ignore restrictions in input: research device only. */
		answer = FUXI_AMFI_DYLD_FEATURES;
		return copyout(&answer, query.output, sizeof(answer));
	}
	case FUXI_AMFI_BOOTARG_STATE:
		answer = FUXI_AMFI_NOT_ENFORCING;
		return copyout(&answer, arg, sizeof(answer));
	default:
		return ENOPOLICY;
	}
}

/* Saved exec strings: executable path (padded), argv, then environment. */
static int
fuxi_container_from_strings(const char *strings, size_t size, int argc,
    user_addr_t path, user_size_t capacity)
{
	const char key[] = "CFFIXED_USER_HOME=";
	size_t offset = 0;
	if (argc < 0) {
		return ENOENT;
	}
	for (int i = 0; i <= argc; ++i) {
		if (offset >= size) {
			return ENOENT;
		}
		size_t len = strnlen(strings + offset, size - offset);
		if (len == size - offset) {
			return ENOENT;
		}
		offset += len + 1;
		if (i == 0) {
			while (offset < size && strings[offset] == 0) {
				offset++;
			}
		}
	}
	while (offset < size) {
		const char *entry = strings + offset;
		size_t len = strnlen(entry, size - offset);
		if (len == size - offset) {
			break;
		}
		if (len > sizeof(key) - 1 && !memcmp(entry, key, sizeof(key) - 1)) {
			const char *home = entry + sizeof(key) - 1;
			size_t bytes = len - (sizeof(key) - 1) + 1;
			if (*home != '/' || bytes > MAXPATHLEN) {
				return ENOENT;
			}
			return bytes > capacity ? ENAMETOOLONG : copyout(home, path, bytes);
		}
		offset += len + 1;
	}
	return ENOENT; /* No container assigned; never invent a shared container. */
}

static int
fuxi_sandbox_container(user_addr_t arg)
{
	struct fuxi_container_query query;
	int error = copyin(arg, &query, sizeof(query));
	if (error) {
		return error;
	}
	if (query.pid <= 0 || query.pid > INT_MAX) {
		return ESRCH;
	}
	proc_t target = proc_find((pid_t)query.pid);
	if (target == PROC_NULL) {
		return ESRCH;
	}
	if (query.check_version && (uint64_t)proc_pidversion(target) != query.version) {
		proc_rele(target);
		return ESRCH;
	}
	user_addr_t top = target->user_stack;
	size_t length = target->p_argslen;
	int argc = target->p_argc;
	task_t task = proc_task(target);
	if (!top || !length || length > NCARGS + PAGE_SIZE || task == TASK_NULL) {
		proc_rele(target);
		return ENOENT;
	}
	/* Retain the task before dropping proc, then take a map reference. */
	task_reference(task);
	proc_rele(target);
	vm_map_t map = get_task_map_reference(task);
	task_deallocate(task);
	if (map == VM_MAP_NULL) {
		return ENOENT;
	}
	vm_size_t span = vm_map_round_page(length, vm_map_page_mask(map));
	vm_offset_t buffer = 0;
	vm_map_copy_t copy = VM_MAP_COPY_NULL;
	if (top < span) {
		error = EIO;
	} else if (kmem_alloc(kernel_map, &buffer, span,
	    KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_BSD) != KERN_SUCCESS) {
		error = ENOMEM;
	} else if (vm_map_copyin(map, top - span, span, FALSE, &copy) != KERN_SUCCESS) {
		error = EIO;
	} else if (vm_map_copy_overwrite(kernel_map, buffer, copy, span, FALSE) != KERN_SUCCESS) {
		error = EIO;
	} else {
		copy = VM_MAP_COPY_NULL; /* consumed by successful overwrite */
		error = fuxi_container_from_strings((const char *)(buffer + span - length),
		    length, argc, query.path, query.capacity);
	}
	if (copy != VM_MAP_COPY_NULL) {
		vm_map_copy_discard(copy);
	}
	if (buffer) {
		kmem_free(kernel_map, buffer, span);
	}
	vm_map_deallocate(map);
	return error;
}

static const struct mac_policy_ops fuxi_amfi_ops = {
	.mpo_policy_syscall = fuxi_amfi_syscall,
};
static struct mac_policy_conf fuxi_amfi_conf = {
	.mpc_name = "AMFI",
	.mpc_fullname = "FUXI research AMFI queries",
	.mpc_ops = &fuxi_amfi_ops,
};
static mac_policy_handle_t fuxi_amfi_handle;

static void
fuxi_amfi_policy_register(void)
{
	if (!fuxi_amfi_enabled) {
		printf("FUXI AMFI: disabled (fuxiamfi=0)\n");
		return;
	}
	int error = mac_policy_register(&fuxi_amfi_conf, &fuxi_amfi_handle, NULL);
	printf("FUXI AMFI: query policy %s (%d), dyld=0xff boot-state=0x86\n",
	    error ? "NOT registered" : "registered", error);
}
#endif /* FUXI_AMFI_H */
