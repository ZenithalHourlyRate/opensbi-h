#include <sbi/sbi_ptw.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_unpriv.h>
#include <sbi/sbi_hart.h>
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_asm.h>

struct sbi_ptw_mode {
	sbi_load_pte_func load_pte;
	bool addr_signed;
	char parts[8];
};

static sbi_pte_t sbi_load_pte_pa(sbi_addr_t addr, const struct sbi_ptw_csr *csr,
				 struct sbi_trap_info *trap);

static sbi_pte_t sbi_load_pte_gpa(sbi_addr_t addr,
				  const struct sbi_ptw_csr *csr,
				  struct sbi_trap_info *trap);

static struct sbi_ptw_mode sbi_ptw_sv39x4 = { .load_pte	   = sbi_load_pte_pa,
					      .addr_signed = false,
					      .parts = { 12, 9, 9, 11, 0 } };

static struct sbi_ptw_mode sbi_ptw_sv39 = { .load_pte	 = sbi_load_pte_gpa,
					    .addr_signed = true,
					    .parts	 = { 12, 9, 9, 9, 0 } };

static sbi_pte_t sbi_load_pte_pa(sbi_addr_t addr, const struct sbi_ptw_csr *csr,
				 struct sbi_trap_info *trap)
{
	register ulong tinfo asm("a3");

	register ulong mtvec = sbi_hart_expected_trap_addr();
	sbi_pte_t ret	     = 0;
	trap->cause	     = 0;

	asm volatile(
		/* clang-format off */
		"add %[tinfo], %[taddr], zero\n"
		"csrrw %[mtvec], " STR(CSR_MTVEC) ", %[mtvec]\n"
		".option push\n"
		".option norvc\n"
		REG_L " %[ret], %[addr]\n"
		".option pop\n"
		"csrw " STR(CSR_MTVEC) ", %[mtvec]"
		/* clang-format on */
		: [mtvec] "+&r"(mtvec), [tinfo] "+&r"(tinfo), [ret] "=&r"(ret)
		: [addr] "m"(*(sbi_pte_t *)addr), [taddr] "r"((ulong)trap)
		: "a4", "memory");
	return ret;
}

static sbi_pte_t sbi_load_pte_gpa(sbi_addr_t addr,
				  const struct sbi_ptw_csr *csr,
				  struct sbi_trap_info *trap)
{
	trap->cause = 0;
	return 0;
}

static inline bool addr_valid(sbi_addr_t addr, const struct sbi_ptw_mode *mode,
			      int va_bits)
{
	if (mode->addr_signed) {
		int64_t a = ((int64_t)addr) >> (va_bits - 1);
		return a == 0 || a == -1;
	} else {
		return (addr >> va_bits) == 0;
	}
}

static int sbi_pt_walk(sbi_addr_t addr, sbi_addr_t pt_root,
		       const struct sbi_ptw_csr *csr,
		       const struct sbi_ptw_mode *mode, struct sbi_ptw_out *out,
		       struct sbi_trap_info *trap)
{
	int num_levels = 0, va_bits = 0;
	int level, shift;
	sbi_addr_t node, addr_part, mask;
	sbi_pte_t pte;

	while (mode->parts[num_levels]) {
		va_bits += mode->parts[num_levels];
		num_levels++;
	}

	if (!addr_valid(addr, mode, va_bits)) {
		trap->cause = CAUSE_LOAD_PAGE_FAULT;
		trap->tinst = 0;
		trap->tval  = 0;
		trap->tval2 = 0;

		return SBI_EINVAL;
	}

	shift = va_bits;
	node  = pt_root;

	for (level = num_levels - 1; level >= 1; level--) {
		shift -= mode->parts[level];
		mask	  = (1UL << mode->parts[level]) - 1;
		addr_part = (addr >> shift) & mask;

		sbi_printf("%s: load pte 0x%lx\n", __func__,
			   node + addr_part * sizeof(sbi_pte_t));

		pte = mode->load_pte(node + addr_part * sizeof(sbi_pte_t), csr,
				     trap);

		if (trap->cause) {
			sbi_printf("%s: load pte failed %ld\n", __func__,
				   trap->cause);
			return SBI_EINVAL;
		}

		sbi_printf("%s: pte is %016lx\n", __func__, pte);

		if ((pte & 1) != 1) {
			sbi_printf("%s: pte not valid\n", __func__);
			trap->cause = CAUSE_LOAD_PAGE_FAULT;
			trap->tinst = 0;
			trap->tval  = 0;
			trap->tval2 = 0;

			return SBI_EINVAL;
		}

		sbi_panic("%s: todo", __func__);
	}

	sbi_panic("dunno");
}

static ulong convert_pf_to_gpf(ulong cause)
{
	switch (cause) {
	case CAUSE_LOAD_PAGE_FAULT:
		return CAUSE_LOAD_GUEST_PAGE_FAULT;
	case CAUSE_STORE_PAGE_FAULT:
		return CAUSE_STORE_GUEST_PAGE_FAULT;
	case CAUSE_FETCH_PAGE_FAULT:
		return CAUSE_FETCH_GUEST_PAGE_FAULT;
	default:
		return cause;
	}
}

int sbi_ptw_translate(sbi_addr_t gva, const struct sbi_ptw_csr *csr,
		      struct sbi_ptw_out *out, struct sbi_trap_info *trap)
{
	int ret;
	if (csr->vsatp >> SATP_MODE_SHIFT != SATP_MODE_OFF) {
		sbi_panic("not bare");
	}

	if (csr->hgatp >> HGATP_MODE_SHIFT != HGATP_MODE_SV39X4) {
		sbi_panic("not sv39x4");
	}

	ret = sbi_pt_walk(gva, (csr->hgatp & HGATP_PPN) << PAGE_SHIFT, csr,
			  &sbi_ptw_sv39x4, out, trap);

	trap->cause = convert_pf_to_gpf(trap->cause);
	return ret;
}