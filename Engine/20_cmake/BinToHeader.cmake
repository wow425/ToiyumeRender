file(READ ${INPUT_FILE} HEX_CONTENT HEX)

string(LENGTH "${HEX_CONTENT}" HEX_LENGTH)

math(EXPR LAST_INDEX "${HEX_LENGTH} - 2")

get_filename_component(FILE_NAME ${INPUT_FILE} NAME_WE)

set(OUTPUT_TEXT
"// ============================================\n"
"// Auto Generated Shader Binary\n"
"// ============================================\n\n"
"unsigned char ${FILE_NAME}_cso[] = {\n"
)

foreach(i RANGE 0 ${LAST_INDEX} 2)

    string(SUBSTRING "${HEX_CONTENT}" ${i} 2 HEX_BYTE)

    set(OUTPUT_TEXT
        "${OUTPUT_TEXT}0x${HEX_BYTE},"
    )

endforeach()

set(OUTPUT_TEXT
"${OUTPUT_TEXT}\n};\n"
)

file(WRITE ${OUTPUT_FILE} "${OUTPUT_TEXT}")
