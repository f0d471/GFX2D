/******************************************************************************
* gfx2d.ld: 项目链接脚本（板上目标）
*
* 底本：Main/sdk/bsp/efinix/EfxSapphireSoc/linker/default.ld
*       该文件是 sdk 的「已登记修改」项之一（见 Main/sdk/README.md）：
*       程序区从官方的 124K 改为 4092K，并加了超过 512KB 就链接报错的断言。
*
* 与底本的差异，只有两处：
*   1. 栈：__stack_size 的缺省值 4K -> 64K。游戏与执行器要用几百字节的
*      调用深度加整帧的临时缓冲，4K 不够；栈就放在 .stack 段里，
*      大小写死在这里，不再吃 sdk 的缺省值。
*   2. 文件头这段说明。
* 其余（分区、段顺序、__global_pointer$、断言、各 PROVIDE 符号）与底本逐行相同，
* 改动时请对照底本，不要顺手重新排版。
*
* 为什么要有自己的一份：sdk/ 是 SoC IP 的生成物，重新生成 IP 时整个目录会被
* 覆盖，已登记的修改要逐条恢复；项目自己的链接脚本放在 sw/ 下就不会被碰到。
* 见 Engineering/Todo/sw/SW1-软件平台分层与双目标构建.md 2.3 节。
*
* 判据：编译出来的 .map 里 .stack 为 64KB（SW1 第 3 节）。
******************************************************************************/

OUTPUT_ARCH( "riscv" )

ENTRY( _start )

MEMORY
{
  ram  (wxai!r) : ORIGIN = 0x00001000, LENGTH = 4092K
}

PHDRS
{
  ram PT_LOAD;
}

SECTIONS
{
  __stack_size = DEFINED(__stack_size) ? __stack_size : 64K;

  .init           :
  {
    KEEP (*(SORT_NONE(.init)))
  } >ram AT>ram :ram

  .text           :
  {
    *(.text.Proc_1);
    *(.text.Proc_2);
    *(.text.Proc_3);
    *(.text.Proc_4);
    *(.text.Proc_5);
    *(.text.Proc_6);
    *(.text.Proc_7);
    *(.text.Proc_8);
    *(.text.Func_1);
    *(.text.Func_2);
    *(.text.Func_3);
    *(.text.strcpy);
    *libc.a:*(.text .text.*)
    *(.text.main);
    *(.text.unlikely .text.unlikely.*)
    *(.text.startup .text.startup.*)
    *(.text .text.*)
    *(.gnu.linkonce.t.*)
    *(.note.gnu.build-id)
  } >ram AT>ram :ram

  .fini           :
  {
    KEEP (*(SORT_NONE(.fini)))
  } >ram AT>ram :ram

  PROVIDE (__etext = .);
  PROVIDE (_etext = .);
  PROVIDE (etext = .);

  . = ALIGN(4);

  .preinit_array  :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >ram AT>ram :ram

  .init_array     :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
    KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >ram AT>ram :ram

  .fini_array     :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
    KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >ram AT>ram :ram

  .ctors          :
  {
    /* gcc uses crtbegin.o to find the start of
       the constructors, so we make sure it is
       first.  Because this is a wildcard, it
       doesn't matter if the user does not
       actually link against crtbegin.o; the
       linker won't look for a file to match a
       wildcard.  The wildcard also means that it
       doesn't matter which directory crtbegin.o
       is in.  */
    KEEP (*crtbegin.o(.ctors))
    KEEP (*crtbegin?.o(.ctors))
    /* We don't want to include the .ctor section from
       the crtend.o file until after the sorted ctors.
       The .ctor section from the crtend file contains the
       end of ctors marker and it must be last */
    KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
    KEEP (*(SORT(.ctors.*)))
    KEEP (*(.ctors))
  } >ram AT>ram :ram

  .dtors          :
  {
    KEEP (*crtbegin.o(.dtors))
    KEEP (*crtbegin?.o(.dtors))
    KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
    KEEP (*(SORT(.dtors.*)))
    KEEP (*(.dtors))
  } >ram AT>ram :ram

  .lalign         : ALIGN(4)
  {
    PROVIDE( _data_lma = . );
  } >ram AT>ram :ram

  .dalign         : ALIGN(4)
  {
    PROVIDE( _data = . );
  } >ram AT>ram :ram

  .data          :
  {
    *(.rdata)
    *(.rodata .rodata.*)
    *(.gnu.linkonce.r.*)
    *(.data .data.*)
    *(.gnu.linkonce.d.*)
    . = ALIGN(8);
    PROVIDE( __global_pointer$ = . + 0x7F0 );
    *(.sdata .sdata.*)
    *(.gnu.linkonce.s.*)
    . = ALIGN(8);
    *(.srodata.cst16)
    *(.srodata.cst8)
    *(.srodata.cst4)
    *(.srodata.cst2)
    *(.srodata .srodata.*)
    . += 10; /* fix for linker false error message */
  } >ram AT>ram :ram

  . = ALIGN(4);
  PROVIDE( _edata = . );
  PROVIDE( edata = . );

  /* bootloader 只从 Flash 拷 USER_SOFTWARE_SIZE 字节，镜像更大会被静默截断 */
  __app_image_max = DEFINED(__app_image_max) ? __app_image_max : 512K;
  ASSERT(_edata - ORIGIN(ram) <= __app_image_max, "application image exceeds bootloader USER_SOFTWARE_SIZE")

  PROVIDE( _fbss = . );
  PROVIDE( __bss_start = . );
  .bss            :
  {
    *(.sbss*)
    *(.gnu.linkonce.sb.*)
    *(.bss .bss.*)
    *(.gnu.linkonce.b.*)
    *(COMMON)
    . = ALIGN(4);
  } >ram AT>ram :ram

  . = ALIGN(8);
  PROVIDE( _end = . );
  PROVIDE( end = . );

  .stack  : ALIGN(16)
  {
    PROVIDE( _heap_end = . );
    . = __stack_size;
    PROVIDE( _sp = . );
	__freertos_irq_stack_top = .;
  } >ram AT>ram :ram
}
