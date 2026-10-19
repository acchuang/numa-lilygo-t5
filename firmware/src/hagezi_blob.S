.section .rodata
.global hagezi_hashes_start
.type hagezi_hashes_start, @object
.align 4
hagezi_hashes_start:
.incbin "include/hagezi_hashes.bin"
.global hagezi_hashes_end
hagezi_hashes_end:
