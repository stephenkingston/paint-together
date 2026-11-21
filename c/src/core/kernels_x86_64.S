/*
 * Paint (C port) - the kernels of kernels.h for x86-64: Linux (ELF, SysV), Intel
 * Macs (Mach-O, SysV) and Windows x64 (PE). kernels.c is the specification: the
 * same bytes out and the same state handed back.
 *
 * Baseline x86-64 only (SSE2; no SSSE3, SSE4, AVX or BMI), so that it runs on
 * any 64-bit PC. The bodies take SysV arguments; on Windows each function's entry
 * goes through k_win64, which moves the arguments over and keeps the registers
 * Windows wants kept (rdi, rsi, xmm6-xmm15). No red zone, and no data: the tables
 * are made on the stack.
 */
	.intel_syntax noprefix

#if defined(__APPLE__)
#define L(x) L##x
#define CALLH(n) call _##n
#define JMPH(n) jmp _##n
.macro FN name
	.text
	.p2align 4
	.globl _\name
_\name:
.endm
.macro HELPER name
	.text
	.p2align 4
_\name:
.endm
#elif defined(_WIN32)
#define L(x) .L##x
#define CALLH(n) call n
#define JMPH(n) jmp n
.macro FN name
	.section .text$\name,"xr"
	.p2align 4
	.globl \name
	.def \name; .scl 2; .type 32; .endef
\name:
.endm
.macro HELPER name
	.section .text$\name,"xr"
	.p2align 4
\name:
.endm
#else
#define L(x) .L##x
#define CALLH(n) call n
#define JMPH(n) jmp n
.macro FN name
	.section .text.\name,"ax",@progbits
	.p2align 4
	.globl \name
	.type \name,@function
\name:
.endm
.macro HELPER name
	.section .text.\name,"ax",@progbits
	.p2align 4
	.type \name,@function
\name:
.endm
#endif

/* Windows: into the SysV body, through k_win64 */
#if defined(_WIN32)
#define WIN(body) lea rax, [rip + body]; jmp k_win64
#else
#define WIN(body)
#endif

#if defined(_WIN32)
/* Calls the body in rax with the Windows arguments (rcx, rdx, r8, r9, then the
 * stack) moved to SysV's (rdi, rsi, rdx, rcx, r8, r9), keeping rdi, rsi and xmm6-15 */
HELPER k_win64
	push rdi
	push rsi
	sub rsp, 168
	movdqa [rsp], xmm6
	movdqa [rsp + 16], xmm7
	movdqa [rsp + 32], xmm8
	movdqa [rsp + 48], xmm9
	movdqa [rsp + 64], xmm10
	movdqa [rsp + 80], xmm11
	movdqa [rsp + 96], xmm12
	movdqa [rsp + 112], xmm13
	movdqa [rsp + 128], xmm14
	movdqa [rsp + 144], xmm15
	mov rdi, rcx
	mov rsi, rdx
	mov rdx, r8
	mov rcx, r9
	mov r8, [rsp + 224]
	mov r9, [rsp + 232]
	call rax
	movdqa xmm6, [rsp]
	movdqa xmm7, [rsp + 16]
	movdqa xmm8, [rsp + 32]
	movdqa xmm9, [rsp + 48]
	movdqa xmm10, [rsp + 64]
	movdqa xmm11, [rsp + 80]
	movdqa xmm12, [rsp + 96]
	movdqa xmm13, [rsp + 112]
	movdqa xmm14, [rsp + 128]
	movdqa xmm15, [rsp + 144]
	add rsp, 168
	pop rsi
	pop rdi
	ret
#endif

/* ---- deflate ----
 * int k_deflate(k_deflate_t *s)
 * rsi data, r8 out, r9 bits, r10d nbits, r11d i, r12d len, r13d best, r14d at,
 * r15d match limit, rbx/rbp a chain (from, to); frame: [rsp] s, [rsp+8] end - K_ROOM,
 * [rsp+16] hash, [rsp+24..280) bytes bit-reversed */
#define REV 24
FN k_deflate
	WIN(L(df))
L(df):
	push rbx
	push rbp
	push r12
	push r13
	push r14
	push r15
	sub rsp, 280
	mov [rsp], rdi
	mov rax, [rdi + 16]
	sub rax, 16
	mov [rsp + 8], rax
	xor eax, eax
L(df_t):
	mov edx, eax
	mov ecx, 8
	xor ebx, ebx
L(df_tb):
	shr edx, 1
	adc ebx, ebx
	dec ecx
	jnz L(df_tb)
	mov [rsp + rax + REV], bl
	inc al
	jnz L(df_t)
	mov rsi, [rdi]
	mov r8, [rdi + 8]
	mov r9, [rdi + 24]
	mov r10d, [rdi + 32]
	mov r11d, [rdi + 36]
	mov r12d, [rdi + 40]
L(df_loop):
	cmp r8, [rsp + 8]
	ja L(df_full)
	cmp r11d, r12d
	jge L(df_eob)
	mov r13d, 3
	or r14d, -1
	lea eax, [r12 - 3]
	cmp r11d, eax
	jge L(df_emit)
	/* the positions with the same hash: the longest match (the latest of equals) */
	mov eax, [rsi + r11]
	call L(df_hash)
	mov [rsp + 16], eax
	call L(df_chain)
	mov r15d, r12d
	sub r15d, r11d
	mov eax, 258
	cmp r15d, eax
	cmova r15d, eax
L(df_scan):
	cmp rbx, rbp
	jae L(df_scanned)
	mov eax, [rbx]
	add rbx, 4
	lea ecx, [r11 - 32768]
	cmp eax, ecx
	jle L(df_scan)
	lea rcx, [rsi + rax]
	lea rdx, [rsi + r11]
	movzx edi, byte ptr [rcx + r13 - 1]
	cmp dil, [rdx + r13 - 1]
	jne L(df_scan)
	call L(df_match)
	cmp eax, r13d
	jb L(df_scan)
	mov r13d, eax
	mov r14d, [rbx - 4]
	jmp L(df_scan)
L(df_scanned):
	/* this position joins its chain; a full chain keeps its newer half */
	mov rdi, [rsp]
	mov eax, [rsp + 16]
	mov ecx, [rdi + 44]
	mov edx, eax
	imul edx, ecx
	mov rbx, [rdi + 48]
	lea rbx, [rbx + rdx*4]
	mov rdx, [rdi + 56]
	movzx ebp, word ptr [rdx + rax*2]
	cmp ebp, ecx
	jne L(df_ins)
	shr ecx, 1
	mov ebp, ecx
	push rsi
	lea rsi, [rbx + rcx*4]
	mov rdi, rbx
	rep movsd
	pop rsi
L(df_ins):
	mov [rbx + rbp*4], r11d
	inc ebp
	mov [rdx + rax*2], bp
	/* a longer match from the next byte? then this one is a literal */
	test r14d, r14d
	js L(df_emit)
	mov r15d, r12d
	sub r15d, r11d
	dec r15d
	mov eax, 258
	cmp r15d, eax
	cmova r15d, eax
	cmp r13d, r15d
	jae L(df_emit)
	mov eax, [rsi + r11 + 1]
	call L(df_hash)
	call L(df_chain)
L(df_lazy):
	cmp rbx, rbp
	jae L(df_emit)
	mov eax, [rbx]
	add rbx, 4
	lea ecx, [r11 - 32767]
	cmp eax, ecx
	jle L(df_lazy)
	lea rcx, [rsi + rax]
	lea rdx, [rsi + r11 + 1]
	movzx edi, byte ptr [rcx + r13]
	cmp dil, [rdx + r13]
	jne L(df_lazy)
	call L(df_match)
	cmp eax, r13d
	jbe L(df_lazy)
	or r14d, -1
L(df_emit):
	test r14d, r14d
	js L(df_lit)
	/* length: symbol j (257 + j) and its extra bits */
	lea eax, [r13 - 3]
	xor ecx, ecx
	mov edx, eax
	cmp r13d, 258
	jne L(df_m1)
	mov edx, 28
	jmp L(df_m2)
L(df_m1):
	cmp eax, 8
	jb L(df_m2)
	bsr ecx, eax
	sub ecx, 2
	mov edx, eax
	shr edx, cl
	and edx, 3
	lea edx, [rdx + rcx*4 + 4]
L(df_m2):
	call L(df_extra)
	cmp edx, 22
	ja L(df_m3)
	movzx eax, byte ptr [rsp + rdx + REV + 1]
	shr eax, 1
	mov edi, 7
	jmp L(df_m4)
L(df_m3):
	movzx eax, byte ptr [rsp + rdx + REV + 169]
	mov edi, 8
L(df_m4):
	call L(df_put2)
	/* distance: code and extra bits */
	mov eax, r11d
	sub eax, r14d
	dec eax
	xor ecx, ecx
	mov edx, eax
	cmp eax, 2
	jb L(df_d1)
	bsr ecx, eax
	dec ecx
	mov edx, eax
	shr edx, cl
	and edx, 1
	lea edx, [rdx + rcx*2 + 2]
L(df_d1):
	call L(df_extra)
	movzx eax, byte ptr [rsp + rdx + REV]
	shr eax, 3
	mov edi, 5
	call L(df_put2)
	add r11d, r13d
	jmp L(df_flush)
L(df_lit):
	movzx eax, byte ptr [rsi + r11]
	inc r11d
	cmp eax, 144
	jae L(df_lit9)
	movzx eax, byte ptr [rsp + rax + REV + 0x30]
	mov edi, 8
	jmp L(df_put)
L(df_lit9):
	movzx eax, byte ptr [rsp + rax + REV]
	lea eax, [rax*2 + 1]
	mov edi, 9
L(df_put):
	mov ecx, r10d
	shl rax, cl
	or r9, rax
	add r10d, edi
L(df_flush):
	mov [r8], r9
	mov ecx, r10d
	shr ecx, 3
	add r8, rcx
	mov ecx, r10d
	and ecx, -8
	shr r9, cl
	and r10d, 7
	jmp L(df_loop)
L(df_eob):
	add r10d, 7
	mov eax, 1
	jmp L(df_out)
L(df_full):
	xor eax, eax
L(df_out):
	mov rdi, [rsp]
	mov [rdi + 8], r8
	mov [rdi + 24], r9
	mov [rdi + 32], r10d
	mov [rdi + 36], r11d
	add rsp, 280
	pop r15
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret

/* eax: 3 bytes (and one more) -> their hash, as stb's (clobbers edx) */
L(df_hash):
	and eax, 0xFFFFFF
	mov edx, eax
	shl edx, 3
	xor eax, edx
	mov edx, eax
	shr edx, 5
	add eax, edx
	mov edx, eax
	shl edx, 4
	xor eax, edx
	mov edx, eax
	shr edx, 17
	add eax, edx
	mov edx, eax
	shl edx, 25
	xor eax, edx
	mov edx, eax
	shr edx, 6
	add eax, edx
	and eax, 16383
	ret

/* eax: a hash -> rbx, rbp: its chain's positions (from, to) */
L(df_chain):
	mov rdi, [rsp + 8]
	mov rdx, [rdi + 56]
	movzx ebp, word ptr [rdx + rax*2]
	imul eax, [rdi + 44]
	mov rbx, [rdi + 48]
	lea rbx, [rbx + rax*4]
	lea rbp, [rbx + rbp*4]
	ret

/* rcx, rdx: two places -> eax: how many bytes match, up to r15d (clobbers edi, xmm0-1) */
L(df_match):
	xor eax, eax
L(df_m16):
	lea edi, [rax + 16]
	cmp edi, r15d
	ja L(df_mb)
	movdqu xmm0, [rcx + rax]
	movdqu xmm1, [rdx + rax]
	pcmpeqb xmm0, xmm1
	pmovmskb edi, xmm0
	xor edi, 0xFFFF
	jnz L(df_mdiff)
	add eax, 16
	jmp L(df_m16)
L(df_mdiff):
	bsf edi, edi
	add eax, edi
	ret
L(df_mb):
	cmp eax, r15d
	jae L(df_mret)
	movzx edi, byte ptr [rcx + rax]
	cmp dil, [rdx + rax]
	jne L(df_mret)
	inc eax
	jmp L(df_mb)
L(df_mret):
	ret

/* eax: a length - 3 or distance - 1, ecx: its extra bits -> ebx: their value, ebp: their count */
L(df_extra):
	mov ebx, 1
	shl ebx, cl
	dec ebx
	and ebx, eax
	mov ebp, ecx
	ret

/* Puts eax (edi bits) and then ebx (ebp bits) */
L(df_put2):
	mov ecx, r10d
	shl rax, cl
	or r9, rax
	add r10d, edi
	mov eax, ebx
	mov ecx, r10d
	shl rax, cl
	or r9, rax
	add r10d, ebp
	ret

/* ---- checksums ---- */

/* uint32_t k_crc32(uint32_t crc, const uint8_t *p, size_t n): four bytes at a time,
 * from four 256-entry tables made on the stack */
FN k_crc32
	WIN(L(crc))
L(crc):
	sub rsp, 2048
	mov [rsp], al           /* (Windows grows the stack a page at a time) */
	sub rsp, 2056
	xor eax, eax
L(crc_t):
	mov r8d, eax
	mov ecx, 8
L(crc_b):
	shr r8d, 1
	jnc L(crc_n)
	xor r8d, 0xEDB88320
L(crc_n):
	dec ecx
	jnz L(crc_b)
	mov [rsp + rax*4], r8d
	inc al
	jnz L(crc_t)
	mov eax, 256
L(crc_t2):
	mov r8d, [rsp + rax*4 - 1024]
	movzx ecx, r8b
	shr r8d, 8
	xor r8d, [rsp + rcx*4]
	mov [rsp + rax*4], r8d
	inc eax
	cmp eax, 1024
	jne L(crc_t2)
	not edi
L(crc4):
	cmp rdx, 4
	jb L(crc1)
	xor edi, [rsi]
	add rsi, 4
	sub rdx, 4
	movzx ecx, dil
	mov eax, [rsp + rcx*4 + 3072]
	mov ecx, edi
	shr ecx, 8
	movzx ecx, cl
	xor eax, [rsp + rcx*4 + 2048]
	mov ecx, edi
	shr ecx, 16
	movzx ecx, cl
	xor eax, [rsp + rcx*4 + 1024]
	shr edi, 24
	xor eax, [rsp + rdi*4]
	mov edi, eax
	jmp L(crc4)
L(crc1):
	test rdx, rdx
	jz L(crc_end)
	movzx ecx, byte ptr [rsi]
	inc rsi
	xor ecx, edi
	movzx ecx, cl
	shr edi, 8
	xor edi, [rsp + rcx*4]
	dec rdx
	jmp L(crc1)
L(crc_end):
	not edi
	mov eax, edi
	add rsp, 4104
	ret

/* uint32_t k_adler32(uint32_t adler, const uint8_t *p, size_t n): 16 bytes at a time
 * in blocks of up to 2 KB. In a block of N bytes: a += sum, b += N a + sum of (N - i) x_i,
 * which is 16 (P + S) - sum of j A_j, where S sums the bytes, P sums S before each
 * 16 bytes, and A_j sums the bytes at j (0-15) in the 16s */
FN k_adler32
	WIN(L(ad))
L(ad):
	mov eax, edi
	test rdx, rdx
	jz L(ad_ret)
	movzx eax, di
	shr edi, 16
	mov r9, rdx
	pxor xmm15, xmm15
	pcmpeqw xmm8, xmm8
	psrlw xmm8, 15          /* 1s */
	movdqa xmm9, xmm8
	pslldq xmm9, 2
	movdqa xmm10, xmm9
	pslldq xmm10, 2
	paddw xmm9, xmm10
	movdqa xmm10, xmm9
	pslldq xmm10, 4
	paddw xmm9, xmm10
	movdqa xmm10, xmm9
	pslldq xmm10, 8
	paddw xmm9, xmm10       /* 0-7 */
	psllw xmm8, 3
	paddw xmm8, xmm9        /* 8-15 */
L(ad_block):
	cmp r9, 16
	jb L(ad_tail)
	mov rcx, r9
	shr rcx, 4
	mov r10d, 128
	cmp rcx, r10
	cmova rcx, r10
	mov r10, rcx
	shl r10, 4
	sub r9, r10
	pxor xmm1, xmm1
	pxor xmm2, xmm2
	pxor xmm3, xmm3
	pxor xmm4, xmm4
L(ad_16):
	movdqu xmm0, [rsi]
	add rsi, 16
	paddq xmm2, xmm1
	movdqa xmm5, xmm0
	psadbw xmm5, xmm15
	paddq xmm1, xmm5
	movdqa xmm5, xmm0
	punpcklbw xmm5, xmm15
	paddw xmm3, xmm5
	punpckhbw xmm0, xmm15
	paddw xmm4, xmm0
	dec rcx
	jnz L(ad_16)
	pshufd xmm5, xmm1, 0x4E
	paddq xmm1, xmm5
	movq r11, xmm1
	pshufd xmm5, xmm2, 0x4E
	paddq xmm2, xmm5
	movq rcx, xmm2
	pmaddwd xmm3, xmm9
	pmaddwd xmm4, xmm8
	paddd xmm3, xmm4
	pshufd xmm4, xmm3, 0x4E
	paddd xmm3, xmm4
	pshufd xmm4, xmm3, 0xB1
	paddd xmm3, xmm4
	movd r8d, xmm3
	imul r10, rax
	add rcx, r11
	shl rcx, 4
	add r10, rcx
	sub r10, r8
	add r10, rdi
	add rax, r11
	call L(ad_mod)
	jmp L(ad_block)
L(ad_tail):
	test r9, r9
	jz L(ad_done)
L(ad_1):
	movzx ecx, byte ptr [rsi]
	inc rsi
	add eax, ecx
	add edi, eax
	dec r9
	jnz L(ad_1)
	mov r10d, edi
	call L(ad_mod)
L(ad_done):
	shl edi, 16
	or eax, edi
L(ad_ret):
	ret
/* rax = rax mod 65521, rdi = r10 mod 65521 */
L(ad_mod):
	mov ecx, 65521
	xor edx, edx
	div rcx
	mov r11, rdx
	mov rax, r10
	xor edx, edx
	div rcx
	mov edi, edx
	mov eax, r11d
	ret

/* ---- PNG rows ----
 * xmm1 a (left), xmm2 b (above), xmm3 c (above left), xmm15 zeros -> xmm6: the
 * Paeth predictor, a byte at a time (clobbers xmm4, xmm5, xmm7, xmm8). The
 * distances are bytes, saturated: pc = pa + pb when a and b are on the same
 * side of c, else |pa - pb|, and a saturated pc still compares the same way. */
HELPER k_paeth
	movdqa xmm4, xmm2
	psubusb xmm4, xmm3
	movdqa xmm5, xmm3
	psubusb xmm5, xmm2
	por xmm5, xmm4          /* pa = |b - c| */
	movdqa xmm6, xmm1
	psubusb xmm6, xmm3
	movdqa xmm7, xmm3
	psubusb xmm7, xmm1
	por xmm7, xmm6          /* pb = |a - c| */
	pcmpeqb xmm4, xmm15
	pcmpeqb xmm6, xmm15
	pcmpeqb xmm4, xmm6      /* a and b on the same side of c */
	movdqa xmm6, xmm5
	psubusb xmm6, xmm7
	movdqa xmm8, xmm7
	psubusb xmm8, xmm5
	por xmm8, xmm6          /* |pa - pb| */
	movdqa xmm6, xmm5
	paddusb xmm6, xmm7      /* pa + pb */
	pand xmm6, xmm4
	pandn xmm4, xmm8
	por xmm4, xmm6          /* pc */
	movdqa xmm6, xmm5
	psubusb xmm6, xmm7
	psubusb xmm5, xmm4
	por xmm6, xmm5
	pcmpeqb xmm6, xmm15     /* pa <= pb and pa <= pc */
	psubusb xmm7, xmm4
	pcmpeqb xmm7, xmm15     /* pb <= pc */
	movdqa xmm8, xmm2
	pand xmm8, xmm7
	pandn xmm7, xmm3
	por xmm7, xmm8          /* b, or c */
	movdqa xmm8, xmm1
	pand xmm8, xmm6
	pandn xmm6, xmm7
	por xmm6, xmm8          /* a, or that */
	ret

/* void k_png_costs(const uint8_t *row, const uint8_t *prev, size_t n, uint32_t cost[5]) */
FN k_png_costs
	WIN(L(pc))
L(pc):
	mov rax, rdi
	mov rdi, rcx
	mov rcx, rdx
	mov rdx, rsi
	mov rsi, rax
	xor r8d, r8d
	JMPH(k_png_rows)

/* void k_png_filter(uint8_t *out, const uint8_t *row, const uint8_t *prev, size_t n, int filter) */
FN k_png_filter
	WIN(L(pf))
L(pf):
	mov r9d, r8d
	mov r8, rdi
	JMPH(k_png_rows)

/* Both: rsi row, rdx prev, rcx n, r8 out (0: the costs, to rdi), r9d filter. 16
 * bytes at a time, then 4; each filter's bytes go to the stack and their |sum| to
 * xmm9-13. xmm14 holds 1s, xmm15 0s. */
.macro CONSUME off, acc
	movdqu [rsp + \off], xmm4
	movdqa xmm5, xmm15
	psubb xmm5, xmm4
	pminub xmm5, xmm4
	psadbw xmm5, xmm15
	paddq \acc, xmm5
.endm
.macro FOLD acc, off
	pshufd xmm4, \acc, 0x4E
	paddq xmm4, \acc
	movd [rdi + \off], xmm4
.endm
HELPER k_png_rows
	sub rsp, 88
	pxor xmm15, xmm15
	pcmpeqb xmm4, xmm4
	pxor xmm14, xmm14
	psubb xmm14, xmm4
	pxor xmm9, xmm9
	pxor xmm10, xmm10
	pxor xmm11, xmm11
	pxor xmm12, xmm12
	pxor xmm13, xmm13
	xor eax, eax
L(pr_loop):
	lea r10, [rax + 16]
	cmp r10, rcx
	ja L(pr_narrow)
	movdqu xmm0, [rsi + rax]
	movdqu xmm2, [rdx + rax]
	test rax, rax
	jz L(pr_first)
	movdqu xmm1, [rsi + rax - 4]
	movdqu xmm3, [rdx + rax - 4]
	jmp L(pr_body)
L(pr_first):
	movdqa xmm1, xmm0
	pslldq xmm1, 4
	movdqa xmm3, xmm2
	pslldq xmm3, 4
	jmp L(pr_body)
L(pr_narrow):
	cmp rax, rcx
	jae L(pr_done)
	movd xmm0, [rsi + rax]
	movd xmm2, [rdx + rax]
	pxor xmm1, xmm1
	pxor xmm3, xmm3
	test rax, rax
	jz L(pr_body)
	movd xmm1, [rsi + rax - 4]
	movd xmm3, [rdx + rax - 4]
L(pr_body):
	movdqa xmm4, xmm0
	CONSUME 0, xmm9
	movdqa xmm4, xmm0
	psubb xmm4, xmm1
	CONSUME 16, xmm10
	movdqa xmm4, xmm0
	psubb xmm4, xmm2
	CONSUME 32, xmm11
	movdqa xmm5, xmm1
	pxor xmm5, xmm2
	pand xmm5, xmm14
	movdqa xmm6, xmm1
	pavgb xmm6, xmm2
	psubb xmm6, xmm5        /* (a + b) >> 1 */
	movdqa xmm4, xmm0
	psubb xmm4, xmm6
	CONSUME 48, xmm12
	CALLH(k_paeth)
	movdqa xmm4, xmm0
	psubb xmm4, xmm6
	CONSUME 64, xmm13
	test r8, r8
	jz L(pr_next)
	mov r11d, r9d
	shl r11d, 4
	movdqu xmm4, [rsp + r11]
	cmp r10, rcx
	ja L(pr_st4)
	movdqu [r8 + rax], xmm4
	jmp L(pr_next)
L(pr_st4):
	movd [r8 + rax], xmm4
L(pr_next):
	add rax, 4
	cmp r10, rcx
	ja L(pr_loop)
	add rax, 12
	jmp L(pr_loop)
L(pr_done):
	test r8, r8
	jnz L(pr_ret)
	FOLD xmm9, 0
	FOLD xmm10, 4
	FOLD xmm11, 8
	FOLD xmm12, 12
	FOLD xmm13, 16
L(pr_ret):
	add rsp, 88
	ret

/* void k_unfilter(uint8_t *cur, const uint8_t *raw, const uint8_t *prior, size_t bpp, size_t n, int filter)
 * rdi cur, rsi raw, rdx prior, rcx bpp, r8 n, r9d filter, rax k. 3 and 4 bytes a
 * pixel: a pixel at a time (4 bytes; a 3-byte pixel's 4th is the next one's first,
 * written again); otherwise, and the last 3-byte pixel, a byte at a time. */
FN k_unfilter
	WIN(L(uf))
L(uf):
	test r9d, r9d
	jnz L(uf1)
	mov rcx, r8
	rep movsb
	ret
L(uf1):
	xor eax, eax
	cmp r9d, 2
	jne L(uf_serial)
L(uf_up):
	lea r10, [rax + 16]
	cmp r10, r8
	ja L(uf_up1)
	movdqu xmm0, [rsi + rax]
	movdqu xmm1, [rdx + rax]
	paddb xmm0, xmm1
	movdqu [rdi + rax], xmm0
	mov rax, r10
	jmp L(uf_up)
L(uf_up1):
	cmp rax, r8
	jae L(uf_ret)
	mov r10b, [rsi + rax]
	add r10b, [rdx + rax]
	mov [rdi + rax], r10b
	inc rax
	jmp L(uf_up1)
L(uf_serial):
	pxor xmm15, xmm15
	pcmpeqb xmm4, xmm4
	pxor xmm14, xmm14
	psubb xmm14, xmm4
	pxor xmm1, xmm1
	pxor xmm3, xmm3
L(uf_next):
	lea r10, [rax + 4]
	lea r11, [rcx - 3]
	cmp r11, 1
	ja L(uf_narrow)
	cmp r10, r8
	ja L(uf_narrow)
	movd xmm0, [rsi + rax]
	pxor xmm2, xmm2
	lea r11d, [r9 - 3]
	cmp r11d, 1
	ja L(uf_w1)
	movd xmm2, [rdx + rax]
L(uf_w1):
	mov r11, rcx
	jmp L(uf_pred)
L(uf_narrow):
	cmp rax, r8
	jae L(uf_ret)
	movzx r10d, byte ptr [rsi + rax]
	movd xmm0, r10d
	pxor xmm1, xmm1
	pxor xmm2, xmm2
	pxor xmm3, xmm3
	lea r11d, [r9 - 3]
	cmp r11d, 1
	ja L(uf_n1)
	movzx r10d, byte ptr [rdx + rax]
	movd xmm2, r10d
L(uf_n1):
	cmp rax, rcx
	jb L(uf_n2)
	mov r10, rax
	sub r10, rcx
	movzx r11d, byte ptr [rdi + r10]
	movd xmm1, r11d
	cmp r9d, 4
	jne L(uf_n2)
	movzx r11d, byte ptr [rdx + r10]
	movd xmm3, r11d
L(uf_n2):
	mov r11d, 1
L(uf_pred):
	movdqa xmm6, xmm1
	cmp r9d, 1
	je L(uf_add)
	cmp r9d, 4
	jne L(uf_avg)
	CALLH(k_paeth)
	jmp L(uf_add)
L(uf_avg):
	movdqa xmm5, xmm1
	pxor xmm5, xmm2
	pand xmm5, xmm14
	pavgb xmm6, xmm2
	psubb xmm6, xmm5
L(uf_add):
	paddb xmm0, xmm6
	cmp r11, 1
	je L(uf_st1)
	movd [rdi + rax], xmm0
	movdqa xmm1, xmm0
	movdqa xmm3, xmm2
	add rax, r11
	jmp L(uf_next)
L(uf_st1):
	movd r10d, xmm0
	mov [rdi + rax], r10b
	inc rax
	jmp L(uf_next)
L(uf_ret):
	ret

/* ---- inflate ----
 * int k_inflate(k_inflate_t *s)
 * rdi s, rsi in, r8 out, r9 bits, r15d how many (nb), r10d how many stb would
 * hold (held), r11/r12 the length and distance tables, r13 in_end - 16,
 * r14 out_end - 274, rbx the table to decode with, rbp the length */
FN k_inflate
	WIN(L(if))
L(if):
	push rbx
	push rbp
	push r12
	push r13
	push r14
	push r15
	mov rsi, [rdi]
	mov r13, [rdi + 8]
	sub r13, 16
	mov r8, [rdi + 16]
	mov r14, [rdi + 32]
	sub r14, 274
	mov r11, [rdi + 40]
	mov r12, [rdi + 48]
	mov r9d, [rdi + 56]
	mov r15d, [rdi + 60]
	mov r10d, r15d
L(if_top):
	cmp rsi, r13
	ja L(if_more)
	cmp r8, r14
	ja L(if_more)
	mov rbx, r11
	call L(if_sym)
	test eax, eax
	js L(if_bad)
	cmp eax, 256
	jae L(if_notlit)
	mov [r8], al
	inc r8
	jmp L(if_top)
L(if_notlit):
	je L(if_end)
	cmp eax, 286
	jae L(if_bad)
	sub eax, 257
	lea ebp, [rax + 3]
	cmp eax, 8
	jb L(if_len)
	mov ebp, 258
	cmp eax, 28
	je L(if_len)
	mov ecx, eax
	shr ecx, 2
	dec ecx
	and eax, 3
	add eax, 4
	shl eax, cl
	lea ebp, [rax + 3]
	call L(if_take)
	add ebp, eax
L(if_len):
	mov rbx, r12
	call L(if_sym)
	cmp eax, 30
	jae L(if_bad)
	lea edx, [rax + 1]
	cmp eax, 4
	jb L(if_dist)
	mov ecx, eax
	shr ecx, 1
	dec ecx
	and eax, 1
	add eax, 2
	shl eax, cl
	lea edx, [rax + 1]
	call L(if_take)
	add edx, eax
L(if_dist):
	mov rax, r8
	sub rax, [rdi + 24]
	cmp rax, rdx
	jb L(if_baddist)
	mov rax, r8
	sub rax, rdx
	lea rbx, [r8 + rbp]
	cmp edx, 16
	jae L(if_c16)
	cmp edx, 8
	jae L(if_c8)
	cmp edx, 1
	je L(if_fill)
	/* 2-7 back: the first m - d bytes one at a time, m the first multiple of d from 8;
	 * then 8 at a time from m back, which is the same bytes */
	mov ecx, edx
L(if_m):
	add ecx, edx
	cmp ecx, 8
	jb L(if_m)
	sub ecx, edx
	xor edx, edx
L(if_b):
	movzx ebp, byte ptr [rax + rdx]
	mov [r8 + rdx], bpl
	inc edx
	cmp edx, ecx
	jb L(if_b)
	add r8, rcx
L(if_c8):
	cmp r8, rbx
	jae L(if_cend)
	movq xmm0, [rax]
	movq [r8], xmm0
	add rax, 8
	add r8, 8
	jmp L(if_c8)
L(if_c16):
	movdqu xmm0, [rax]
	movdqu [r8], xmm0
	add rax, 16
	add r8, 16
	cmp r8, rbx
	jb L(if_c16)
	jmp L(if_cend)
L(if_fill):
	movzx eax, byte ptr [rax]
	imul eax, eax, 0x01010101
	movd xmm0, eax
	pshufd xmm0, xmm0, 0
L(if_f16):
	movdqu [r8], xmm0
	add r8, 16
	cmp r8, rbx
	jb L(if_f16)
L(if_cend):
	mov r8, rbx
	jmp L(if_top)
L(if_more):
	xor eax, eax
	jmp L(if_out)
L(if_end):
	mov eax, 1
	jmp L(if_out)
L(if_bad):
	mov eax, -1
	jmp L(if_out)
L(if_baddist):
	mov eax, -2
L(if_out):
	mov ecx, r15d
	sub ecx, r10d
	shr ecx, 3
	sub rsi, rcx
	mov [rdi], rsi
	mov [rdi + 16], r8
	mov ecx, r10d
	mov edx, 1
	shl rdx, cl
	dec rdx
	and rdx, r9
	mov [rdi + 56], edx
	mov [rdi + 60], r10d
	pop r15
	pop r14
	pop r13
	pop r12
	pop rbp
	pop rbx
	ret

/* Refills to 56 bits or more, and decodes a symbol of the table in rbx, as stb's
 * stbi__zhuffman_decode: eax, or -1 (clobbers ecx, edx) */
L(if_sym):
	mov rax, [rsi]
	mov ecx, r15d
	shl rax, cl
	or r9, rax
	mov eax, 63
	sub eax, r15d
	shr eax, 3
	add rsi, rax
	or r15d, 56
	cmp r10d, 16
	jge L(if_s1)
	mov eax, 32
	sub eax, r10d
	and eax, -8
	add r10d, eax
L(if_s1):
	mov eax, r9d
	and eax, 511
	movzx eax, word ptr [rbx + rax*2]
	test eax, eax
	jz L(if_slow)
	mov ecx, eax
	shr ecx, 9
	and eax, 511
L(if_eat):
	shr r9, cl
	sub r15d, ecx
	sub r10d, ecx
	ret
L(if_slow):
	mov edx, r9d
	mov ecx, 16
L(if_rev):
	shr edx, 1
	adc eax, eax
	dec ecx
	jnz L(if_rev)
	mov ecx, 10
L(if_len2):
	cmp eax, [rbx + rcx*4 + 1056]
	jl L(if_found)
	inc ecx
	jmp L(if_len2)
L(if_found):
	cmp ecx, 16
	jae L(if_bad2)
	mov edx, ecx
	neg ecx
	add ecx, 16
	shr eax, cl
	mov ecx, edx
	movzx edx, word ptr [rbx + rcx*2 + 1024]
	sub eax, edx
	movzx edx, word ptr [rbx + rcx*2 + 1124]
	add eax, edx
	cmp eax, 288
	jge L(if_bad2)
	movsxd rdx, eax
	cmp byte ptr [rbx + rdx + 1156], cl
	jne L(if_bad2)
	movzx eax, word ptr [rbx + rdx*2 + 1444]
	jmp L(if_eat)
L(if_bad2):
	mov eax, -1
	ret

/* ecx extra bits -> eax: their value (stb's stbi__zreceive; keeps edx) */
L(if_take):
	cmp r10d, ecx
	jge L(if_t1)
	mov eax, 32
	sub eax, r10d
	and eax, -8
	add r10d, eax
L(if_t1):
	mov eax, 1
	shl eax, cl
	dec eax
	and eax, r9d
	shr r9, cl
	sub r15d, ecx
	sub r10d, ecx
	ret

#if defined(__APPLE__)
	.subsections_via_symbols
#elif !defined(_WIN32)
	.section .note.GNU-stack,"",@progbits
#endif
