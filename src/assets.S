/* Binary assets embedded into the executable (Wii and PC builds). */
	.section .rodata
	.balign 4

	.global asset_font_medium
asset_font_medium:
	.incbin "assets/fonts/RoundedMplus-Medium.ttf"
	.global asset_font_medium_end
asset_font_medium_end:

	.balign 4
	.global asset_font_bold
asset_font_bold:
	.incbin "assets/fonts/RoundedMplus-Bold.ttf"
	.global asset_font_bold_end
asset_font_bold_end:

#if defined(__linux__) && defined(__ELF__)
	.section .note.GNU-stack,"",%progbits
#endif
