/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "opt-A3.h"
#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <copyinout.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

#if OPT_A3
// map vaddr to paddr and keep track of permissions
//struct pagetable {
//	vaddr_t vaddr;
//	paddr_t paddr;
//	bool readable;
//	bool writable;
//	bool executable;
//};

// coremap shoud store:
// which frames are used and available
// contiguous mem allocations
struct coremap {
	bool available;
	int numContiguous;
	paddr_t address;
};

struct coremap *coremap;
bool coremapLoaded = false;
int numFrames;

void
vm_bootstrap(void)
{
	// call ram_getsize to get remaining physical mem
	paddr_t lo, hi;
	ram_getsize(&lo, &hi);

	// logically partition into fixed size frames
	numFrames = (hi - lo) / PAGE_SIZE;

	// store coremap at start of ram_getsize
	coremap = (struct coremap *)PADDR_TO_KVADDR(lo);
	lo = ROUNDUP(lo + numFrames * (sizeof(struct coremap)), PAGE_SIZE);

	// update usable frames
	numFrames = (hi - lo) / PAGE_SIZE;

	// initialize coremap
	paddr_t page = lo;
	for (int i = 0; i < numFrames; i++) {
		coremap[i].available = true;
		coremap[i].numContiguous = 0;
		coremap[i].address = page;
		page += PAGE_SIZE;
	}

	// coremap has finished initialization
	coremapLoaded = true;
}
#else
void
vm_bootstrap(void)
{
	/* Do nothing. */
}
#endif

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

	spinlock_acquire(&stealmem_lock);

#if OPT_A3
	if (!coremapLoaded) {
		// still starting up, just stealmem
		addr = ram_stealmem(npages);
		spinlock_release(&stealmem_lock);
		return addr;
	} else {
		// must allocate a contiguous block
		for (int i = 0; i < numFrames; i++) {
			int start = i; // save starting index
			bool canAlloc = true; // assume continuous block
			if (coremap[i].available) {
				// check next npages
				for (int k = 0; k < (int)npages; k++) {
					i = start + k;
					if (i >= numFrames || !coremap[i].available) {
						canAlloc = false;
						break;
					}
				}
				// contiguous block found
				if (canAlloc) {
					for (int k = 0; k < (int)npages; k++) {
						coremap[start + k].available = false;
					}
					coremap[start].numContiguous = (int)npages;
					spinlock_release(&stealmem_lock);
					return coremap[start].address;
				}
			}
		}
		// could not allocate, out of mem
		spinlock_release(&stealmem_lock);
		return ENOMEM;
	}
#else
	addr = ram_stealmem(npages);

	spinlock_release(&stealmem_lock);
	return addr;
#endif
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
#if OPT_A3
	else if (pa == ENOMEM) {
		return ENOMEM;
	}
#endif
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
#if OPT_A3
	spinlock_acquire(&stealmem_lock);

	if (coremapLoaded) {
		for (int i = 0; i < numFrames; i++) {
			if (addr == coremap[i].address) {
				int npages = coremap[i].numContiguous;
				coremap[i].numContiguous = 0;
				for (int k = 0; k < npages; k++) {
					coremap[i + k].available = true;
				}
				break;
			}
		}
	}

	spinlock_release(&stealmem_lock);
#else
	/* nothing - leak the memory. */

	(void)addr;
#endif
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
#if OPT_A3
		// kill the current process, don't panic
		return EINVAL;
#else
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
#endif
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

#if OPT_A3
	vbase1 = as->as_text_vbase;
        vtop1 = vbase1 + as->as_text_npages * PAGE_SIZE;
	vbase2 = as->as_data_vbase;
	vtop2 = vbase2 + as->as_data_npages * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;
	bool readOnly = false;
	
	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		vaddr_t vframe = (faultaddress - vbase1) / PAGE_SIZE; 
		paddr = (faultaddress - vbase1) + as->as_text_ptable[vframe].paddr;
		readOnly = !as->as_text_ptable[vframe].writeable;
	} else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		vaddr_t vframe = (faultaddress - vbase2) / PAGE_SIZE;
		paddr = (faultaddress - vbase2) + as->as_data_ptable[vframe].paddr;
		readOnly = !as->as_data_ptable[vframe].writeable;
	} else if (faultaddress >= stackbase && faultaddress < stacktop) {
		vaddr_t vframe = (faultaddress - stackbase) / PAGE_SIZE;
		paddr = (faultaddress - stackbase) + as->as_stack_ptable[vframe].paddr;
		readOnly = !as->as_stack_ptable[vframe].writeable;
	} else {
	        return EFAULT;
	}
#else

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

#endif

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
#if OPT_A3
		bool full = false;
		if (elo & TLBLO_VALID && i == NUM_TLB - 1) {
			// reached end of TLB, we know it is full
			full = true;
		} else if (elo & TLBLO_VALID) {
			// not at end of TLB but current slot is occupied
			continue;
		}
		ehi = faultaddress;
		if (readOnly && as->as_loaded) {
			// addrspace is done loading and we're in read-only space
			elo = (paddr | TLBLO_VALID) & ~TLBLO_DIRTY;
		} else {
			// addrspace not done loading or we're in writable space
			elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		}
		if (full) {
			// kick something random out
			tlb_random(ehi, elo);
		} else {
			tlb_write(ehi, elo, i);
		}
		splx(spl);
		return 0;
	}
#else
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}
#endif
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

#if OPT_A3
	as->as_text_vbase = 0;
	// as->as_text_ptable;
	as->as_text_npages = 0;
	as->as_data_vbase = 0;
	// as->as_data_ptable;
	as->as_data_npages = 0;
	//as->as_stack_ptable = kmalloc(DUMBVM_STACKPAGES * sizeof(struct pagetable));
	as->as_loaded = false;
#else
	as->as_vbase1 = 0;
	as->as_pbase1 = 0;
	as->as_npages1 = 0;
	as->as_vbase2 = 0;
	as->as_pbase2 = 0;
	as->as_npages2 = 0;
	as->as_stackpbase = 0;
#endif

	return as;
}

void
as_destroy(struct addrspace *as)
{
#if OPT_A3
	// call free_kpages on the frames for each seg
	for (int i = 0; i < (int)as->as_text_npages; i++) {
		free_kpages(as->as_text_ptable[i].paddr);
	}

	for (int i = 0; i < (int)as->as_data_npages; i++) {
		free_kpages(as->as_data_ptable[i].paddr);
	}

	for (int i = 0; i < DUMBVM_STACKPAGES; i++) {
		free_kpages(as->as_stack_ptable[i].paddr);
	}

	// free page tables
	kfree(as->as_text_ptable);
	kfree(as->as_data_ptable);
	kfree(as->as_stack_ptable);
#endif
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;


	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

#if OPT_A3

	// set up text seg
	if (as->as_text_vbase == 0) {
		as->as_text_ptable = kmalloc(npages * sizeof(struct pagetable));
		for (int i = 0; i < (int)npages; i++) {
			as->as_text_ptable[i].readable = readable;
			as->as_text_ptable[i].writeable = writeable;
			as->as_text_ptable[i].executable = executable;
		}
		as->as_text_vbase = vaddr;
		as->as_text_npages = npages;
		return 0;
	}

	// set up data seg
	if (as->as_data_vbase == 0) {
		as->as_data_ptable = kmalloc(npages * sizeof(struct pagetable));
		for (int i = 0; i < (int)npages; i++) {
			as->as_data_ptable[i].readable = readable;
			as->as_data_ptable[i].writeable = writeable;
			as->as_data_ptable[i].executable = executable;
		}
		as->as_data_vbase = vaddr;
		as->as_data_npages = npages;
		return 0;
	}

#else
	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
		return 0;
	}

#endif
	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
#if OPT_A3
	(void)paddr;
	(void)npages;
#else
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
#endif
}

int
as_prepare_load(struct addrspace *as)
{
#if OPT_A3
	// preallocate frames for each page in the segment
	//vaddr_t text_vaddr = as->as_text_vbase;
	for (int i = 0; i < (int)as->as_text_npages; i++) {
		as->as_text_ptable[i].frame = i;
		// allocate each frame one at a time
		as->as_text_ptable[i].paddr = getppages(1);
		if (as->as_text_ptable[i].paddr == 0) {
			return ENOMEM;
		}
	//	text_vaddr += PAGE_SIZE;
	}

	//vaddr_t data_vaddr = as->as_data_vbase;
	for (int i = 0; i < (int)as->as_data_npages; i++) {
		as->as_data_ptable[i].frame = i;
		// allocate each frame one at a time
		as->as_data_ptable[i].paddr = getppages(1);
		if (as->as_data_ptable[i].paddr == 0) {
			return ENOMEM;
		}
	//	data_vaddr += PAGE_SIZE;
	}

/*	for (int i = 0; i < DUMBVM_STACKPAGES; i++) {
		as->as_stack_ptable[i].vaddr = data_vaddr; // virtual memory is contiguous
		// allocate each frame one at a time
		as->as_stack_ptable[i].paddr = getppages(1);
		if (as->as_stack_ptable[i].paddr == 0) {
			return ENOMEM;
		}
		data_vaddr += PAGE_SIZE;
	}*/

	as_zero_region(as->as_text_ptable[0].paddr, as->as_text_npages); // avoid warning

	return 0;
#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);

	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}

	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);

	return 0;
#endif
}

int
as_complete_load(struct addrspace *as)
{
#if OPT_A3
	as->as_loaded = true;
#else
	(void)as;
#endif
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
#if OPT_A3
	// always allocate NUM_STACK_PAGES for the stack
	// need to create a page table for the stack
	as->as_stack_ptable = kmalloc(DUMBVM_STACKPAGES * sizeof(struct pagetable));

	// need to allocate frames for the stack
	//vaddr_t stack_vaddr = USERSTACK;
	for (int i = 0; i < DUMBVM_STACKPAGES; i++) {
		as->as_stack_ptable[i].frame = i;
		// allocate each frame one at a time
		as->as_stack_ptable[i].paddr = getppages(1);
		if (as->as_stack_ptable[i].paddr == 0) {
			return ENOMEM;
		}
	//	stack_vaddr += PAGE_SIZE;
	}

	*stackptr = USERSTACK;
	return 0;
#else
	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;
	return 0;
#endif
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{

	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

#if OPT_A3
	// create segments based on old addrspace
	new->as_text_vbase = old->as_text_vbase;
	new->as_text_npages = old->as_text_npages;
	new->as_data_vbase = old->as_data_vbase;
	new->as_data_npages = old->as_data_npages;

	// alloc frames for segs
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}
	new->as_text_ptable = kmalloc(new->as_text_npages * sizeof(struct pagetable));
	new->as_data_ptable = kmalloc(new->as_data_npages * sizeof(struct pagetable));
	if (as_define_stack(new, (vaddr_t *)USERSTACK)) {
		as_destroy(new);
		return ENOMEM;
	}

	// memcpy frames from old addrspace to frames in new addrspace
	for (int i = 0; i < (int)new->as_text_npages; i++) {
		//copyout(new->as_text_ptable[i], old->as_text_ptable[i], sizeof(struct pagetable));
		memcpy((void *)PADDR_TO_KVADDR(new->as_text_ptable[i].paddr),
			(const void *)PADDR_TO_KVADDR(old->as_text_ptable[i].paddr),
			PAGE_SIZE);
	}
	for (int i = 0; i < (int)new->as_data_npages; i++) {
		//copyout(new->as_data_ptable[i], old->as_data_ptable[i], sizeof(struct pagetable));
		memcpy((void *)PADDR_TO_KVADDR(new->as_text_ptable[i].paddr),
			(const void *)PADDR_TO_KVADDR(old->as_text_ptable[i].paddr),
			PAGE_SIZE);
	}
	for (int i = 0; i < DUMBVM_STACKPAGES; i++) {
		memcpy((void *)PADDR_TO_KVADDR(new->as_stack_ptable[i].paddr),
			(const void *)PADDR_TO_KVADDR(old->as_stack_ptable[i].paddr),
			PAGE_SIZE);
	}

	*ret = new;
	return 0;
#else

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
	
	*ret = new;
	return 0;
#endif
}
