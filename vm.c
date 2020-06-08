#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

static int swapToFile(struct proc *p);
static int findFreePage(struct proc *p);
static int choosePageToSwap(void);
static void addToCurrentPages(uint va);
static int findFreeEntry(void);

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)];
  if(*pde & PTE_P){
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;
  // @TODO: what if it is another proccess?
  struct proc *p = myproc();

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    // Check if can generate more pages
    if(p->pid > 2 && p->ramCounter == 16){
      if(p->swapCounter < 16){
        if(swapToFile(p) < 0)
          panic("allocuvm: swapToFile failed!");
      }
      else
        return 0;
      // @TODO: return 0?
    }

    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
    if(p->pid > 2){
      // Update process paging info
      int indx = findFreePage(p);
      if(indx == -1)
        panic("alocuvm: no free space in ramPages!");
      p->ramPages[indx] = a;
      p->ramCounter++;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;
  struct proc *p = myproc();

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
    if(p && p->pid > 2){
    	int indx = -1;
    	for(int i = 0; i < MAX_PAGES; i++){
    		if(currentPages[i].va == a){
    			indx = i;
    			break;
    		}
    	}
    	//Didn't go throgh copyuvm, or only refrence remained
    	if(indx == -1){
    		char *v = P2V(pa);
			kfree(v);
    	}
    	else if(currentPages[indx].refCounter == 1){
    		char *v = P2V(pa);
			kfree(v);
			currentPages[indx].va = 0;
			currentPages[indx].refCounter = 0;
    	}
    	else if(currentPages[indx].refCounter > 1){
    		currentPages[indx].refCounter--;
    	}
    	else{
    		panic("deallocuvm: refCounter under 1");
    	}
    }
    else{
		char *v = P2V(pa);
		kfree(v);
    }
      
      
      // Clear the place in ram array
      if(p->pgdir == pgdir){
        for (int i = 0; i < 16; ++i){
          if(p->ramPages[i] == a){
            p->ramPages[i] = 0;
            p->ramCounter--;
            break;
          }
        }
      }
      
      *pte = 0;
    }else if((*pte & PTE_PG) != 0){ // In case page is in swapFile
      // Clear the place in swap array
      if(p->pgdir == pgdir){
        for (int i = 0; i < 16; ++i){
          if(p->swapPages[i] == a){
            p->swapPages[i] = 0;
            p->swapCounter--;
            break;
          }
        }
      }
      *pte = 0;
    }
  }
  return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;
  struct proc *p = myproc();

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P)){
      if(*pte & PTE_PG){
        pte = walkpgdir(d, (void*)i, 1); // Create PTE for the page
        // Turn on relevant flags. no physical address
        *pte = PTE_U | PTE_W | PTE_PG;
        continue;
      }
      panic("copyuvm: page not present");
    }
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);

    if(p && p->pid > 2){
	    // COW implemtation
	    if(mappages(d, (void*)i, PGSIZE, pa, flags) < 0)
	    	goto bad;
	    // Update currentPages
	    addToCurrentPages(i);
	    /*
	    // Update PTEs
	    *pte &= PTE_RO;
	    pte_p newPte = walkpgdir(d, (void*)i, 0);
	    if(newPte < 0)
	    	panic("cpyuvm: NEW PTE havent found");
	    *newPte &= PTE_RO;
	    */
	}
	else{ // it's init or shell - ignore COW
		if((mem = kalloc()) == 0)
      		goto bad;
    	memmove(mem, (char*)P2V(pa), PGSIZE);
    	if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      		kfree(mem);
      		goto bad;
    	}
	}
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

// Swap 1 page from RAM to file for given process
// Assume we have free space in swapFile
// page replacement prefrences should be implwmented here
static int
swapToFile(struct proc *p){
  
  // If init or shell
  if(p->pid < 2)
    return -1;

  // Find free space in swap file
  int indx = -1;
  for(int i = 0; i < 16; i++){
    if(p->swapPages[i] == 0){
      indx = i;
      break;
    }
  }
  if(indx == -1)
    return -1;
  
  // Choose from physical memory file to swap
  int ramIndx = choosePageToSwap();
  uint va = p->ramPages[ramIndx];

  // get PTE of chosen page
  pte_t* pte = walkpgdir(p->pgdir, (void*)va, 0);
  
  // Write to file - split because it seems to not work with PGSIZE
  if(writeToSwapFile(p, (char*) P2V(PTE_ADDR(*pte)), indx*PGSIZE, PGSIZE/2) < 0)
    panic("swapToFile: couldnt write to file!");
  if(writeToSwapFile(p, (char*) (P2V(PTE_ADDR(*pte)) + PGSIZE/2), indx*PGSIZE + PGSIZE/2, PGSIZE/2) < 0)
    panic("swapToFile: couldnt write to file!");
  
  // Update paging info
  p->ramPages[ramIndx] = 0;
  p->ramCounter--;
  p->swapPages[indx] = va;
  p->swapCounter++;
  
  // Update PTE info
  *pte &= ~PTE_P;       // Turn off
  *pte |= PTE_PG;       // Turn on
  lcr3(V2P(p->pgdir));  // Flush TLB 

  // Free memory
  kfree((char*) P2V(PTE_ADDR(*pte)));

  return 0;
}

// Find free slot on ramPages array of given process
// if there is no free space return -1
static int
findFreePage(struct proc *p){
  int indx = -1;
  for(int i = 0; i < 16; i++){
    if(p->ramPages[i] == 0){
      indx = i;
      break;
    }
  }
  // cprintf("%s%d\n", "the ramCounter is: ", p->ramCounter);
  // cprintf("%s%d\n", "the free RAM index returned: ", indx);
  return indx;
}

// Check if the file is in swapFile
int
checkIfSwapFault(uint va){
  pde_t* pgdir = myproc()->pgdir;
  pde_t *pde = &pgdir[PDX(va)];
  pte_t *pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
  pte_t * pte = &pgtab[PTX(va)];

  return !(*pte & PTE_P) && (*pte & PTE_PG);
}

// Take page from swap file and move it into physical memory
void
swapToRam(uint va){
  struct proc *p = myproc();
  
  // If init or shell
  if(p->pid < 2)
    panic("swapToRam: process pid <= 2");
  
  // Find page if swapFile
  int indx = -1;
  for(int i = 0; i < 16; i++){
    if(p->swapPages[i] == va){
      indx = i;
      break;
    }
  }
  if(indx == -1)
    panic("swapToRam: requested page not found");
  
  // Save page in temporary buffer
  char buf1[PGSIZE/2] = "";
  char buf2[PGSIZE/2] = "";

  if(readFromSwapFile(p, buf1, indx*PGSIZE, PGSIZE/2) < 0)
    panic("swapToRam: couldnt read from swap file");
   if(readFromSwapFile(p, buf2, indx*PGSIZE + PGSIZE/2, PGSIZE/2) < 0)
    panic("swapToRam: couldnt read from swap file");
  
  // Free space in swap file
  p->swapPages[indx] = 0;
  p->swapCounter--;

  // If RAM is full - swap 1 page to file
  if(p->ramCounter >= 16)
    if(swapToFile(p) < 0)
      panic("swapToRam: swapToFile failed!");
  
  // Find free space in RAM
  indx = -1;
  for(int i = 0; i < 16; i++){
    if(p->ramPages[i] == 0){
      indx = i;
      break;
    }
  }
  if(indx == -1)
    panic("swapToRam: couldnt find free space in RAM");
  
  // Allocate memory in RAM
  char *mem = kalloc();
  if(mem == 0)
    panic("swapToRam: couldnt find free space in RAM");
  memset(mem, 0, PGSIZE);

  // Write from buffer to memory
  memmove(mem, buf1, PGSIZE/2);
  memmove(mem + PGSIZE/2, buf2, PGSIZE/2);
  
  // Update PTE
  pte_t *pte = walkpgdir(p->pgdir, (char*)va, 0);
  *pte = V2P(mem) | ((PTE_FLAGS(*pte) | PTE_P) & ~PTE_PG);
  // *pte = V2P(mem) | PTE_P | PTE_W | PTE_U;
  lcr3(V2P(p->pgdir));  // Flush TLB

  // Update process paging info
  p->ramPages[indx] = va;
  p->ramCounter++;
  
}

// @TODO: implement swapping method
static int
choosePageToSwap(void){
  return 4;
}

// Get virtual address, if exist in currentPages - increase refrence counter
// else - insert it into currentPages with ref counter of 2
static void
addToCurrentPages(uint va){
	int indx = -1;
	for(int i = 0; i < MAX_PAGES; i++){
		if(currentPages[i].va == va){
			indx = i;
			break;
		}
	}
	// If couldn't find
	if(indx == -1){
		int freeEntry = findFreeEntry();
		if(freeEntry < 0)
			panic("addToCurrentPages: no free entry in currentPages");
		// else
		currentPages[freeEntry].va = va;
		currentPages[freeEntry].refCounter = 2; // parent and child
	}
	else{
		currentPages[indx].refCounter++;
	}	
}

// Find free entry in currentPages array
// if failed to find - return -1;
static int
findFreeEntry(void){
	int indx = -1;
	for(int i = 0; i < MAX_PAGES; i++){
		if(currentPages[i].va == 0){
			indx = i;
			break;
		}
	}
	return indx;
}