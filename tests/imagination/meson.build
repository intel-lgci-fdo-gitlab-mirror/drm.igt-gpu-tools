pvr_progs = [ 'pvr_dev_query',
	      'pvr_gem',
	      'pvr_gpu_info',
	      'pvr_heap_info',
	    ]

pvr_deps = test_deps

foreach prog : pvr_progs
	test_executables += executable(prog, prog + '.c',
				       dependencies : pvr_deps,
				       install_dir : pvrdir,
				       install_rpath : pvrdir_rpathdir,
				       install : true)
	test_list += join_paths('pvr', prog)
endforeach
