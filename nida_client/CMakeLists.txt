# Set the template VOL connector name
set (EXE_NAME nida_client)

add_executable(
  ${EXE_NAME}
  nida_client.c
 )

target_link_libraries(
  ${EXE_NAME} 
  log
  )


target_compile_options(${EXE_NAME} PRIVATE  -Wstrict-prototypes -Wall -Wextra -fno-omit-frame-pointer 
  -Wunused -Wuninitialized -Wimplicit-function-declaration -Wshadow 
  -pedantic -pedantic-errors -Wmisleading-indentation -pipe)


