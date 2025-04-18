/*****************************************************************************

Copyright (c) 2010, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/os0file.ic
The interface to the operating system file io

Created 2/20/2010 Jimmy Yang
*******************************************************/

#ifdef UNIV_PFS_IO
/** NOTE! Please use the corresponding macro os_file_create_simple(),
not directly this function!
A performance schema instrumented wrapper function for
os_file_create_simple() which opens or creates a file.
@param[in]	key		Performance Schema Key
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	access_type	OS_FILE_READ_ONLY or OS_FILE_READ_WRITE
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeeded
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return own: handle to the file, not defined if error, error number
can be retrieved with os_file_get_last_error */
UNIV_INLINE
pfs_os_file_t
pfs_os_file_create_simple_func(
	mysql_pfs_key_t key,
	const char*	name,
	os_file_create_t create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success,
	const char*	src_file,
	uint		src_line)
{
	PSI_file_locker_state	state;
	struct PSI_file_locker* locker = NULL;

	/* register a file open or creation depending on "create_mode" */
	register_pfs_file_open_begin(
		&state, locker, key,
		(create_mode == OS_FILE_CREATE)
		? PSI_FILE_CREATE : PSI_FILE_OPEN,
		name, src_file, src_line);

	pfs_os_file_t	file = os_file_create_simple_func(
		name, create_mode, access_type, read_only, success);

	/* Register psi value for the file */
	register_pfs_file_open_end(locker, file,
				   (*success == TRUE ? success : 0));

	return(file);
}

/** NOTE! Please use the corresponding macro
os_file_create_simple_no_error_handling(), not directly this function!
A performance schema instrumented wrapper function for
os_file_create_simple_no_error_handling(). Add instrumentation to
monitor file creation/open.
@param[in]	key		Performance Schema Key
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	OS_FILE_CREATE or OS_FILE_OPEN
@param[in]	access_type	OS_FILE_READ_ONLY, OS_FILE_READ_WRITE, or
				OS_FILE_READ_ALLOW_DELETE; the last option is
				used by a backup program reading the file
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeeded
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return own: handle to the file, not defined if error, error number
can be retrieved with os_file_get_last_error */
UNIV_INLINE
pfs_os_file_t
pfs_os_file_create_simple_no_error_handling_func(
	mysql_pfs_key_t key,
	const char*	name,
	os_file_create_t create_mode,
	ulint		access_type,
	bool		read_only,
	bool*		success,
	const char*	src_file,
	uint		src_line)
{
	PSI_file_locker_state	state;
	struct PSI_file_locker* locker = NULL;

	/* register a file open or creation depending on "create_mode" */
	register_pfs_file_open_begin(
		&state, locker, key,
		create_mode == OS_FILE_CREATE
		? PSI_FILE_CREATE : PSI_FILE_OPEN,
		name, src_file, src_line);

	pfs_os_file_t	file = os_file_create_simple_no_error_handling_func(
		name, create_mode, access_type, read_only, success);

	register_pfs_file_open_end(locker, file,
				 (*success == TRUE ? success : 0));

	return(file);
}

/** NOTE! Please use the corresponding macro os_file_create(), not directly
this function!
A performance schema wrapper function for os_file_create().
Add instrumentation to monitor file creation/open.
@param[in]	key		Performance Schema Key
@param[in]	name		name of the file or path as a null-terminated
				string
@param[in]	create_mode	create mode
@param[in]	read_only	if true read only mode checks are enforced
@param[out]	success		true if succeeded
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return own: handle to the file, not defined if error, error number
can be retrieved with os_file_get_last_error */
UNIV_INLINE
pfs_os_file_t
pfs_os_file_create_func(
	mysql_pfs_key_t key,
	const char*	name,
	os_file_create_t create_mode,
	ulint		type,
	bool		read_only,
	bool*		success,
	const char*	src_file,
	uint		src_line)
{
	PSI_file_locker_state	state;
	struct PSI_file_locker* locker = NULL;

	/* register a file open or creation depending on "create_mode" */
	register_pfs_file_open_begin(
		&state, locker, key,
		create_mode == OS_FILE_CREATE
		? PSI_FILE_CREATE : PSI_FILE_OPEN,
		name, src_file, src_line);

	pfs_os_file_t	file = os_file_create_func(
		name, create_mode, type, read_only, success);

	register_pfs_file_open_end(locker, file,
				(*success == TRUE ? success : 0));

	return(file);
}
/**
NOTE! Please use the corresponding macro os_file_close(), not directly
this function!
A performance schema instrumented wrapper function for os_file_close().
@param[in]	file		handle to a file
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_close_func(
	pfs_os_file_t	file,
	const char*	src_file,
	uint		src_line)
{
	PSI_file_locker_state	state;
	struct PSI_file_locker*	locker = NULL;

	/* register the file close */
	register_pfs_file_io_begin(
		&state, locker, file, 0, PSI_FILE_CLOSE, src_file, src_line);

	bool	result = os_file_close_func(file);

	register_pfs_file_io_end(locker, 0);

	return(result);
}

/** NOTE! Please use the corresponding macro os_file_read(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_read() which requests a synchronous read operation.
@param[in]	type		IO request context
@param[in]	file		Open file handle
@param[out]	buf		buffer where to read
@param[in]	offset		file offset where to read
@param[in]	n		number of bytes to read
@param[out]	o		number of bytes actually read
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return DB_SUCCESS if request was successful */
UNIV_INLINE
dberr_t
pfs_os_file_read_func(
	const IORequest&	type,
	pfs_os_file_t		file,
	void*			buf,
	os_offset_t		offset,
	ulint			n,
	ulint*			o,
	const char*		src_file,
	uint			src_line)
{
	PSI_file_locker_state	state;
	struct PSI_file_locker*	locker = NULL;

	register_pfs_file_io_begin(
		&state, locker, file, n, PSI_FILE_READ, src_file, src_line);

	dberr_t		result;

	result = os_file_read_func(type, file, buf, offset, n, o);

	register_pfs_file_io_end(locker, n);

	return(result);
}

/** NOTE! Please use the corresponding macro os_file_write(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_write() which requests a synchronous write operation.
@param[in]	type		IO request context
@param[in]	name		Name of the file or path as NUL terminated
				string
@param[in]	file		Open file handle
@param[out]	buf		buffer where to read
@param[in]	offset		file offset where to read
@param[in]	n		number of bytes to read
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return	error code
@retval	DB_SUCCESS	if the request was successfully fulfilled */
UNIV_INLINE
dberr_t
pfs_os_file_write_func(
	const IORequest&	type,
	const char*		name,
	pfs_os_file_t		file,
	const void*		buf,
	os_offset_t		offset,
	ulint			n,
	const char*		src_file,
	uint			src_line)
{
	PSI_file_locker_state	state;
	struct PSI_file_locker*	locker = NULL;

	register_pfs_file_io_begin(
		&state, locker, file, n, PSI_FILE_WRITE, src_file, src_line);

	dberr_t		result;

	result = os_file_write_func(type, name, file, buf, offset, n);

	register_pfs_file_io_end(locker, n);

	return(result);
}


/** NOTE! Please use the corresponding macro os_file_flush(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_flush() which flushes the write buffers of a given file to the disk.
Flushes the write buffers of a given file to the disk.
@param[in]	file		Open file handle
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return TRUE if success */
UNIV_INLINE
bool
pfs_os_file_flush_func(
	pfs_os_file_t	file,
	const char*	src_file,
	uint		src_line)
{
	PSI_file_locker_state	state;
	struct PSI_file_locker*	locker = NULL;

	register_pfs_file_io_begin(
		&state, locker, file, 0, PSI_FILE_SYNC, src_file, src_line);

	bool	result = os_file_flush_func(file);

	register_pfs_file_io_end(locker, 0);

	return(result);
}

/** NOTE! Please use the corresponding macro os_file_rename(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_rename()
@param[in]	key		Performance Schema Key
@param[in]	oldpath		old file path as a null-terminated string
@param[in]	newpath		new file path
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_rename_func(
	mysql_pfs_key_t	key,
	const char*	oldpath,
	const char*	newpath,
	const char*	src_file,
	uint		src_line)

{
	PSI_file_locker_state	state;
	struct PSI_file_locker*	locker = NULL;

	register_pfs_file_rename_begin(
		&state, locker, key, PSI_FILE_RENAME, newpath,
		src_file, src_line);

	bool	result = os_file_rename_func(oldpath, newpath);

	register_pfs_file_rename_end(locker, oldpath, newpath, !result);

	return(result);
}

/** NOTE! Please use the corresponding macro os_file_delete(), not directly
this function!
This is the performance schema instrumented wrapper function for
os_file_delete()
@param[in]	key		Performance Schema Key
@param[in]	name		old file path as a null-terminated string
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_delete_func(
	mysql_pfs_key_t	key,
	const char*	name,
	const char*	src_file,
	uint		src_line)
{
	PSI_file_locker_state	state;
	struct PSI_file_locker*	locker = NULL;

	register_pfs_file_close_begin(
		&state, locker, key, PSI_FILE_DELETE, name, src_file, src_line);

	bool	result = os_file_delete_func(name);

	register_pfs_file_close_end(locker, 0);

	return(result);
}

/**
NOTE! Please use the corresponding macro os_file_delete_if_exists(), not
directly this function!
This is the performance schema instrumented wrapper function for
os_file_delete_if_exists()
@param[in]	key		Performance Schema Key
@param[in]	name		old file path as a null-terminated string
@param[in]	exist		indicate if file pre-exist
@param[in]	src_file	file name where func invoked
@param[in]	src_line	line where the func invoked
@return true if success */
UNIV_INLINE
bool
pfs_os_file_delete_if_exists_func(
	mysql_pfs_key_t	key,
	const char*	name,
	bool*		exist,
	const char*	src_file,
	uint		src_line)
{
	PSI_file_locker_state	state;
	struct PSI_file_locker*	locker = NULL;

	register_pfs_file_close_begin(
		&state, locker, key, PSI_FILE_DELETE, name, src_file, src_line);

	bool	result = os_file_delete_if_exists_func(name, exist);

	register_pfs_file_close_end(locker, 0);

	return(result);
}
#endif /* UNIV_PFS_IO */
