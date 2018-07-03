#include "lib.h"
#include <mmu.h>
#include <env.h>

#define TMPPAGE         (BY2PG)
#define TMPPAGETOP      (TMPPAGE+BY2PG)



/* Overview:
 *   Check whether it is a ELF file.
 *
 * Pre-Condition:
 *   binary must longer than 4 byte.
 *
 * Post-Condition:
 *   Return 0 if `binary` isn't an elf. Otherwise
 * return 1.
 */
int is_elf(u_char *binary)
{
        Elf32_Ehdr *ehdr = (Elf32_Ehdr *)binary;

        if (ehdr->e_ident[0] == EI_MAG0 &&
                ehdr->e_ident[1] == EI_MAG1 &&
                ehdr->e_ident[2] == EI_MAG2 &&
                ehdr->e_ident[3] == EI_MAG3) {
                return 0;
        }

        return 1;
}

/* Overview:
 *   load an elf format binary file. Map all section
 * at correct virtual address.
 *
 * Pre-Condition:
 *   `binary` can't be NULL and `size` is the size of binary.
 *
 * Post-Condition:
 *   Return 0 if success. Otherwise return < 0.
 *   If success, the entry point of `binary` will be stored in `start`
 */
int load_prog(u_char *binary, u_int child_envid, int fd)
{
        Elf32_Ehdr *ehdr = (Elf32_Ehdr *)binary;
        Elf32_Phdr *phdr = NULL;
        /* As a loader, we just care about segment,
         * so we just parse program headers.
         */
        u_char *ptr_ph_table = NULL;
        Elf32_Half ph_entry_count;
        Elf32_Half ph_entry_size;
        int i, r, text_start;
        u_int *blk;
        // check whether `binary` is a ELF file.
        if (!is_elf(binary)) {
            writef("Target not in ELF Format!\n");
            return -1;
        }

        ptr_ph_table = binary + ehdr->e_phoff;
        ph_entry_count = ehdr->e_phnum;
        ph_entry_size = ehdr->e_phentsize;

        while (ph_entry_count--) {
                phdr = (Elf32_Phdr *)ptr_ph_table;

                if (phdr->p_type == PT_LOAD) {

                    text_start = 0;
                    for(i = phdr->p_offset;i < phdr->p_filesz + phdr->p_offset;i += BY2PG)
                    {
                        if((r = read_map(fd, i, &blk)) < 0)
                        {
                            writef("Spawn Mapping Text Segment Error with %08x!\n", i);
                            return r;
                        }
                        if((r = phdr->p_filesz + phdr->p_offset - i) < BY2PG)
                        {
                            while(r < BY2PG)
                            {
                                ((char *)blk)[r++] = 0;
                            }
                        }
                        syscall_mem_map(0, blk, child_envid, UTEXT + text_start, PTE_V | PTE_R);
                        text_start += BY2PG;
                    }
                    
                    while(i < phdr->p_memsz + phdr->p_offset)
                    {
                        syscall_mem_alloc(child_envid, UTEXT + text_start, PTE_V | PTE_R);
                        i += BY2PG;
                        text_start+=BY2PG;
                    }
                }

                ptr_ph_table += ph_entry_size;
        }
        return 0;
}

int
init_stack(u_int child, char **argv, u_int *init_esp)
{
        int argc, i, r, tot;
        char *strings;
        u_int *args;

        // Count the number of arguments (argc)
        // and the total amount of space needed for strings (tot)
        tot = 0;
        for (argc=0; argv[argc]; argc++)
                tot += strlen(argv[argc])+1;

        // Make sure everything will fit in the initial stack page
        if (ROUND(tot, 4)+4*(argc+3) > BY2PG)
                return -E_NO_MEM;

        // Determine where to place the strings and the args array
        strings = (char*)TMPPAGETOP - tot;
        args = (u_int*)(TMPPAGETOP - ROUND(tot, 4) - 4*(argc+1));

        if ((r = syscall_mem_alloc(0, TMPPAGE, PTE_V|PTE_R)) < 0)
                return r;
        // Replace this with your code to:
        //
        //      - copy the argument strings into the stack page at 'strings'
        char *ctemp,*argv_temp;
        u_int j;
        ctemp = strings;
        for(i = 0;i < argc; i++)
        {
                argv_temp = argv[i];
                for(j=0;j < strlen(argv[i]);j++)
                {
                        *ctemp = *argv_temp;
                        ctemp++;
                        argv_temp++;
                }
                *ctemp = 0;
                ctemp++;
        }
        //      - initialize args[0..argc-1] to be pointers to these strings
        //        that will be valid addresses for the child environment
        //        (for whom this page will be at USTACKTOP-BY2PG!).
        ctemp = (char *)(USTACKTOP - TMPPAGETOP + (u_int)strings);
        for(i = 0;i < argc;i++)
        {
                args[i] = (u_int)ctemp;
                ctemp += strlen(argv[i])+1;
        }
        //      - set args[argc] to 0 to null-terminate the args array.
        ctemp--;
        args[argc] = ctemp;
        //      - push two more words onto the child's stack below 'args',
        //        containing the argc and argv parameters to be passed
        //        to the child's umain() function.
        u_int *pargv_ptr;
        pargv_ptr = args - 1;
        *pargv_ptr = USTACKTOP - TMPPAGETOP + (u_int)args;
        pargv_ptr--;
        *pargv_ptr = argc;
        //
        //      - set *init_esp to the initial stack pointer for the child
        //
        *init_esp = USTACKTOP - TMPPAGETOP + (u_int)pargv_ptr;
//      *init_esp = USTACKTOP;  // Change this!

        if ((r = syscall_mem_map(0, TMPPAGE, child, USTACKTOP-BY2PG, PTE_V|PTE_R)) < 0)
                goto error;
        if ((r = syscall_mem_unmap(0, TMPPAGE)) < 0)
                goto error;

        return 0;

error:
        syscall_mem_unmap(0, TMPPAGE);
        return r;
}


int spawn(char *prog, char **argv)
{
        int r;
        int fd;
        u_int child_envid;
        int size, text_start;
        u_int i, *blk;
        u_int esp;
    char curpath[MAXPATHLEN] = {'\0'};
    char prefix[] = "user_";
        // Your code begins here

        // Step 1: Open the file specified by `prog` (prog is the path of the program)
        //syscall_curpath(curpath, 0);
    strcat(curpath, "/");
    strcat(curpath, prog);
    if(curpath[strlen(curpath) - 1] != 'b' || curpath[strlen(curpath) - 2] != '.')
        strcat(curpath, ".b");
    if((fd = open(curpath, O_RDWR)) < 0)
    {
        writef("Spawn Open %s Failed with %d\n", prog, fd);
        return fd;
    }
    
    // Step 2: Allocate an env (Hint: using syscall_env_alloc())
        if((child_envid = syscall_env_alloc()) < 0)
    {
        user_panic("Spawn Wrong Env %d", child_envid);
        return child_envid;
    }
    
    // Step 3: Using init_stack(...) to initialize the stack of the allocated env
        init_stack(child_envid, argv, &esp);
    
    // Step 3: Map file's content to new env's text segment
    if((r = read_map(fd, 0, &blk)) < 0)
    {
        writef("Spawn Mapping Text Segment Error with %08x!\n", i);
        return r;
    }
    load_prog(blk, child_envid, fd);
    /*
        size = ((struct Filefd *)num2fd(fd))->f_file.f_size;

    text_start = 0;
    //writef("\nSpawn Size is %08x\n", size);
    for(i = 0x1000;i < size;i += BY2PG)
    {
        if((r = read_map(fd, i, &blk)) < 0)
        {
            writef("Spawn Mapping Text Segment Error with %08x!\n", i);
            return r;
        }
        syscall_mem_map(0, blk, child_envid, UTEXT + text_start, PTE_V | PTE_R);
        text_start += BY2PG;
    }
    */
    //        Hint 1: what is the offset of the text segment in file? try to use objdump to find out.
        //        Hint 2: using read_map(...)
        // Note: Step 1 and 2 need sanity check. In other words, you should check whether
        //       the file is opened successfully, and env is allocated successfully.

        // Your code ends here

        struct Trapframe *tf;
        //writef("\n::::::::::spawn size : %x  sp : %x::::::::\n",size,esp);
        tf = &(envs[ENVX(child_envid)].env_tf);
        tf->pc = UTEXT;
        tf->regs[29]=esp;
    for(i = 0;i < strlen(prog);i++)
    {
        if(prog[i] == '.')
        {
            prog[i] = '\0';
            break;
        }
    }
    strcat(prefix, prog);
    strcpy(&(envs[ENVX(child_envid)]).env_name, prefix);

        // Share memory
        u_int pdeno = 0;
        u_int pteno = 0;
        u_int pn = 0;
        u_int va = 0;
        for(pdeno = 0;pdeno<PDX(UTOP);pdeno++)
        {
                if(!((* vpd)[pdeno]&PTE_V))
                        continue;
                for(pteno = 0;pteno<=PTX(~0);pteno++)
                {
                        pn = (pdeno<<10)+pteno;
                        if(((* vpt)[pn]&PTE_V)&&((* vpt)[pn]&PTE_LIBRARY))
                        {
                                va = pn*BY2PG;

                                if((r = syscall_mem_map(0,va,child_envid,va,(PTE_V|PTE_R|PTE_LIBRARY)))<0)
                                {

                                        writef("va: %x   child_envid: %x   \n",va,child_envid);
                                        user_panic("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@");
                                        return r;
                                }
                        }
                }
        }


        if((r = syscall_set_env_status(child_envid, ENV_RUNNABLE)) < 0)
        {
                writef("set child runnable is wrong\n");
                return r;
        }
        return child_envid;

}

int
spawnl(char *prog, char *args, ...)
{
        return spawn(prog, &args);
}