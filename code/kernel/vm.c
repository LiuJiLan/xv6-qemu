//
//  vm.c
//  xv6-qemu
//
//  Created by LiuJiLan on 2022/4/27.
//

#include "vm.h"
#include "memlayout.h"
#include "kalloc.h"
#include "string.h"
#include "riscv.h"

#define PA2PTE(pa) ((((uint64)pa) >> 12) << 10) //  后10位perm
#define PTE2PA(pte) (((pte) >> 10) << 12)

#define PTE_FLAGS(pte) ((pte) & 0x3FF)  //  准确说是PTE_FLAGS_MASK

#define PXMASK          0x1FF // 9 bits
#define PXSHIFT(level)  (PGSHIFT+(9*(level)))
#define PX(level, va) ((((uint64) (va)) >> PXSHIFT(level)) & PXMASK)

#define SATP_SV39 (8L << 60)

#define LOAD_SATP(pagetable) (SATP_SV39 | (((uint64)pagetable) >> 12))

//  成功返回0, 失败返回-1
//  有可能因为没法分配空间而失败
//  也有可能因为remap而失败
int vm_map(pgtbl_t pgtbl, uptr_t va, uptr_t pa, int perm) {
    //  va, pa都要求页大小对齐
    pte_t * pte = NULL;
    
    for (int level = 2; level > 0; level--) {
        pte = &pgtbl[PX(level, va)];
        if (*pte & PTE_V) {
            pgtbl = (pgtbl_t)(P2V(PTE2PA(*pte)));
            //进入下一级的页表
        } else {
            if ( (pgtbl = (pte_t *)kalloc()) == NULL ) {
                return -1;
            }
            //  alloc下一级的页表
            //  注意此时pgtbl用的地址是内核空间的虚拟地址
            memset(pgtbl, 0, PGSIZE);
            *pte = PA2PTE(V2P(pgtbl)) | PTE_V;
        }
    }
    
    pte = &pgtbl[PX(0, va)];
    
    if (*pte & PTE_V) {
        return -1;  //  remap
    }
    
    *pte = PA2PTE(pa) | PTE_V | perm;
    
    return 0;
}

//  返回NULL表示not mapped
//  LATEX format
//  inverse_of_map() = $vm_map^{-1}$()
//  返回的是物理地址
void * vm_inverse_of_map(pgtbl_t pgtbl, uptr_t va) {
    pte_t * pte = NULL;
    
    for (int level = 2; level > 0; level--) {
        pte = &pgtbl[PX(level, va)];
        if (*pte & PTE_V) {
            pgtbl = (pgtbl_t)(P2V(PTE2PA(*pte)));
            //进入下一级的页表
        } else {
            return NULL;
        }
    }
    
    pte = &pgtbl[PX(0, va)];
    return (void *)(PTE2PA(*pte));
}

//  返回0成功, 返回-1表示本身未映射
//  注意, 不关心被解除映射的表项所在的页表是否完全为空
//  即使完全为空也不去回收, 默认程序员会自己去检查这一点
//  默认程序员会在合适的时候回收
int vm_unmap(pgtbl_t pgtbl, uptr_t va) {
    pte_t * pte = NULL;
    
    for (int level = 2; level > 0; level--) {
        pte = &pgtbl[PX(level, va)];
        if (*pte & PTE_V) {
            pgtbl = (pgtbl_t)(P2V(PTE2PA(*pte)));
            //进入下一级的页表
        } else {
            return -1;
        }
    }
    
    pte = &pgtbl[PX(0, va)];
    
    if ( !(*pte & PTE_V) ) {
        return -1;
    }
    
    kfree(P2V(PTE2PA(*pte)));
    *pte = 0;
    return 0;
}


//  for kernel page table
pgtbl_t kernel_pgtbl;

void kvminit(void) {
    kernel_pgtbl = (pgtbl_t)kalloc();
    memset(kernel_pgtbl, 0, PGSIZE);
}

//  成功返回0, 失败返回-1
//  永远平移, 所以不用提供va
int kvmmap(uptr_t pa, uint64 sz, int perm) {
    uptr_t pstart = PGROUNDDOWN(pa);
    uptr_t pend = PGROUNDDOWN(pa + sz - 1);
    uptr_t va = P2V_WO(pstart);
    
    while (1) {
        if (vm_map(kernel_pgtbl, va, pstart, perm) != 0) {
            return -1;
        }
        
        if (pstart == pend) {
            break;
        }
        
        pstart += PGSIZE;
        va += PGSIZE;
    }
    
    return 0;
}

void vm_2_kpgtbl(void) {
    w_satp(LOAD_SATP(V2P(kernel_pgtbl)));
    sfence_vma();
}


//  for user (in fact, process) page table

void vm_shallow_copy(pgtbl_t upgtbl) {
    for (int i = PX(2, V_P_DIFF); i < 512; i++) {
        upgtbl[i] = kernel_pgtbl[i];
    }
}

pgtbl_t vm_init_upgtbl(void) {
    //  alloc空间
    //  只是初始化进程的页表, 高地址复制
    pgtbl_t ret = (void *)kalloc();
    
    if (!ret) {
        return NULL;
    }
    
    vm_shallow_copy(ret);
    
    return ret;
}

void vm_recursive_cleanup(pte_t * pte) {
    if ( !(*pte & PTE_V) ) {
        //  如果表项V == 0, 直接返回
        return;
    }
    
    if ( !(*pte & (PTE_X | PTE_R | PTE_W)) ) {
        //  V == 1 && 非叶子页表
        pgtbl_t pgtbl = (pgtbl_t)(P2V(PTE2PA(*pte)));
        for (int i = 0; i < 512; i++) {
            vm_recursive_cleanup(&pgtbl[i]);
        }
        kfree( (char*)pgtbl );
        return;
    }
    
    //  V == 1 && 叶子页表
    kfree( (char*)(P2V((PTE2PA(*pte)))) );
    return;
}

void vm_delete_upgtbl(pgtbl_t upgtbl) {
    int end = PX(2, V_P_DIFF);
    for (int i = 0; i < end; i++) {
        vm_recursive_cleanup(&upgtbl[i]);
    }
}