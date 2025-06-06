/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Helpers used for SEV guests
 *
 */
#ifndef SELFTEST_KVM_SEV_H
#define SELFTEST_KVM_SEV_H

#include <stdint.h>
#include <stdbool.h>

#include "linux/psp-sev.h"

#include "kvm_util.h"
#include "svm_util.h"
#include "processor.h"

enum sev_guest_state {
	SEV_GUEST_STATE_UNINITIALIZED = 0,
	SEV_GUEST_STATE_LAUNCH_UPDATE,
	SEV_GUEST_STATE_LAUNCH_SECRET,
	SEV_GUEST_STATE_RUNNING,
};

#define SEV_POLICY_NO_DBG	(1UL << 0)
#define SEV_POLICY_ES		(1UL << 2)

#define SNP_POLICY_SMT		(1ULL << 16)
#define SNP_POLICY_RSVD_MBO	(1ULL << 17)
#define SNP_POLICY_DBG		(1ULL << 19)

#define GHCB_MSR_TERM_REQ	0x100

static inline bool is_sev_snp_vm(struct kvm_vm *vm)
{
	return vm->type == KVM_X86_SNP_VM;
}

static inline bool is_sev_es_vm(struct kvm_vm *vm)
{
	return is_sev_snp_vm(vm) || vm->type == KVM_X86_SEV_ES_VM;
}

static inline bool is_sev_vm(struct kvm_vm *vm)
{
	return is_sev_es_vm(vm) || vm->type == KVM_X86_SEV_VM;
}

void sev_vm_launch(struct kvm_vm *vm, uint32_t policy);
void sev_vm_launch_measure(struct kvm_vm *vm, uint8_t *measurement);
void sev_vm_launch_finish(struct kvm_vm *vm);
void snp_vm_launch_start(struct kvm_vm *vm, uint64_t policy);
void snp_vm_launch_update(struct kvm_vm *vm);
void snp_vm_launch_finish(struct kvm_vm *vm);

struct kvm_vm *vm_sev_create_with_one_vcpu(uint32_t type, void *guest_code,
					   struct kvm_vcpu **cpu);
void vm_sev_launch(struct kvm_vm *vm, uint64_t policy, uint8_t *measurement);

kvm_static_assert(SEV_RET_SUCCESS == 0);

/*
 * A SEV-SNP VM requires the policy reserved bit to always be set.
 * The SMT policy bit is also required to be set based on SMT being
 * available and active on the system.
 */
static inline u64 snp_default_policy(void)
{
	return SNP_POLICY_RSVD_MBO | (is_smt_on() ? SNP_POLICY_SMT : 0);
}

/*
 * The KVM_MEMORY_ENCRYPT_OP uAPI is utter garbage and takes an "unsigned long"
 * instead of a proper struct.  The size of the parameter is embedded in the
 * ioctl number, i.e. is ABI and thus immutable.  Hack around the mess by
 * creating an overlay to pass in an "unsigned long" without a cast (casting
 * will make the compiler unhappy due to dereferencing an aliased pointer).
 */
#define __vm_sev_ioctl(vm, cmd, arg)					\
({									\
	int r;								\
									\
	union {								\
		struct kvm_sev_cmd c;					\
		unsigned long raw;					\
	} sev_cmd = { .c = {						\
		.id = (cmd),						\
		.data = (uint64_t)(arg),				\
		.sev_fd = (vm)->arch.sev_fd,				\
	} };								\
									\
	r = __vm_ioctl(vm, KVM_MEMORY_ENCRYPT_OP, &sev_cmd.raw);	\
	r ?: sev_cmd.c.error;						\
})

#define vm_sev_ioctl(vm, cmd, arg)					\
({									\
	int ret = __vm_sev_ioctl(vm, cmd, arg);				\
									\
	__TEST_ASSERT_VM_VCPU_IOCTL(!ret, #cmd,	ret, vm);		\
})

void sev_vm_init(struct kvm_vm *vm);
void sev_es_vm_init(struct kvm_vm *vm);
void snp_vm_init(struct kvm_vm *vm);

static inline void vmgexit(void)
{
	__asm__ __volatile__("rep; vmmcall");
}

static inline void sev_register_encrypted_memory(struct kvm_vm *vm,
						 struct userspace_mem_region *region)
{
	struct kvm_enc_region range = {
		.addr = region->region.userspace_addr,
		.size = region->region.memory_size,
	};

	vm_ioctl(vm, KVM_MEMORY_ENCRYPT_REG_REGION, &range);
}

static inline void sev_launch_update_data(struct kvm_vm *vm, vm_paddr_t gpa,
					  uint64_t size)
{
	struct kvm_sev_launch_update_data update_data = {
		.uaddr = (unsigned long)addr_gpa2hva(vm, gpa),
		.len = size,
	};

	vm_sev_ioctl(vm, KVM_SEV_LAUNCH_UPDATE_DATA, &update_data);
}

static inline void snp_launch_update_data(struct kvm_vm *vm, vm_paddr_t gpa,
					  uint64_t hva, uint64_t size, uint8_t type)
{
	struct kvm_sev_snp_launch_update update_data = {
		.uaddr = hva,
		.gfn_start = gpa >> PAGE_SHIFT,
		.len = size,
		.type = type,
	};

	vm_sev_ioctl(vm, KVM_SEV_SNP_LAUNCH_UPDATE, &update_data);
}

#endif /* SELFTEST_KVM_SEV_H */
