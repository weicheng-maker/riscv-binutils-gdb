/* Read dbx symbol tables and convert to internal format, for GDB.
   Copyright 1986, 1987, 1988, 1989, 1990, 1991, 1992, 1993
   Free Software Foundation, Inc.

This file is part of GDB.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.  */

/* This module provides three functions: dbx_symfile_init,
   which initializes to read a symbol file; dbx_new_init, which 
   discards existing cached information when all symbols are being
   discarded; and dbx_symfile_read, which reads a symbol table
   from a file.

   dbx_symfile_read only does the minimum work necessary for letting the
   user "name" things symbolically; it does not read the entire symtab.
   Instead, it reads the external and static symbols and puts them in partial
   symbol tables.  When more extensive information is requested of a
   file, the corresponding partial symbol table is mutated into a full
   fledged symbol table by going back and reading the symbols
   for real.  dbx_psymtab_to_symtab() is the function that does this */

#include "defs.h"
#include <string.h>

#if defined(USG) || defined(__CYGNUSCLIB__)
#include <sys/types.h>
#include <fcntl.h>
#endif

#include <obstack.h>
#include <sys/param.h>
#ifndef	NO_SYS_FILE
#include <sys/file.h>
#endif
#include <sys/stat.h>
#include <ctype.h>
#include "symtab.h"
#include "breakpoint.h"
#include "command.h"
#include "target.h"
#include "gdbcore.h"		/* for bfd stuff */
#include "libbfd.h"		/* FIXME Secret internal BFD stuff (bfd_read) */
#include "libaout.h"	 	/* FIXME Secret internal BFD stuff for a.out */
#include "symfile.h"
#include "objfiles.h"
#include "buildsym.h"
#include "stabsread.h"
#include "gdb-stabs.h"
#include "demangle.h"
#include "language.h"		/* Needed inside partial-stab.h */
#include "complaints.h"

#include "aout/aout64.h"
#include "aout/stab_gnu.h"	/* We always use GNU stabs, not native, now */

#if !defined (SEEK_SET)
#define SEEK_SET 0
#define SEEK_CUR 1
#endif

/* Each partial symbol table entry contains a pointer to private data for the
   read_symtab() function to use when expanding a partial symbol table entry
   to a full symbol table entry.

   For dbxread this structure contains the offset within the file symbol table
   of first local symbol for this file, and length (in bytes) of the section
   of the symbol table devoted to this file's symbols (actually, the section
   bracketed may contain more than just this file's symbols).  It also contains
   further information needed to locate the symbols if they are in an ELF file.

   If ldsymlen is 0, the only reason for this thing's existence is the
   dependency list.  Nothing else will happen when it is read in.  */

#define LDSYMOFF(p) (((struct symloc *)((p)->read_symtab_private))->ldsymoff)
#define LDSYMLEN(p) (((struct symloc *)((p)->read_symtab_private))->ldsymlen)
#define SYMLOC(p) ((struct symloc *)((p)->read_symtab_private))
#define SYMBOL_SIZE(p) (SYMLOC(p)->symbol_size)
#define SYMBOL_OFFSET(p) (SYMLOC(p)->symbol_offset)
#define STRING_OFFSET(p) (SYMLOC(p)->string_offset)
#define FILE_STRING_OFFSET(p) (SYMLOC(p)->file_string_offset)

struct symloc {
  int ldsymoff;
  int ldsymlen;
  int symbol_size;
  int symbol_offset;
  int string_offset;
  int file_string_offset;
};

/* Macro to determine which symbols to ignore when reading the first symbol
   of a file.  Some machines override this definition. */
#ifndef IGNORE_SYMBOL
/* This code is used on Ultrix systems.  Ignore it */
#define IGNORE_SYMBOL(type)  (type == (int)N_NSYMS)
#endif

/* Remember what we deduced to be the source language of this psymtab. */

static enum language psymtab_language = language_unknown;

/* Nonzero means give verbose info on gdb action.  From main.c.  */
extern int info_verbose;

/* The BFD for this file -- implicit parameter to next_symbol_text.  */

static bfd *symfile_bfd;

/* The size of each symbol in the symbol file (in external form).
   This is set by dbx_symfile_read when building psymtabs, and by
   dbx_psymtab_to_symtab when building symtabs.  */

static unsigned symbol_size;

/* This is the offset of the symbol table in the executable file */
static unsigned symbol_table_offset;

/* This is the offset of the string table in the executable file */
static unsigned string_table_offset;

/* For elf+stab executables, the n_strx field is not a simple index
   into the string table.  Instead, each .o file has a base offset
   in the string table, and the associated symbols contain offsets
   from this base.  The following two variables contain the base
   offset for the current and next .o files. */
static unsigned int file_string_table_offset;
static unsigned int next_file_string_table_offset;

/* This is the lowest text address we have yet encountered.  */
static CORE_ADDR lowest_text_address;

/* Complaints about the symbols we have encountered.  */

struct complaint lbrac_complaint = 
  {"bad block start address patched", 0, 0};

struct complaint string_table_offset_complaint =
  {"bad string table offset in symbol %d", 0, 0};

struct complaint unknown_symtype_complaint =
  {"unknown symbol type %s", 0, 0};

struct complaint unknown_symchar_complaint =
  {"unknown symbol descriptor `%c'", 0, 0};

struct complaint lbrac_rbrac_complaint =
  {"block start larger than block end", 0, 0};

struct complaint lbrac_unmatched_complaint =
  {"unmatched N_LBRAC before symtab pos %d", 0, 0};

struct complaint lbrac_mismatch_complaint =
  {"N_LBRAC/N_RBRAC symbol mismatch at symtab pos %d", 0, 0};

struct complaint repeated_header_complaint =
  {"\"repeated\" header file not previously seen, at symtab pos %d", 0, 0};

struct complaint repeated_header_name_complaint =
  {"\"repeated\" header file not previously seen, named %s", 0, 0};

/* During initial symbol readin, we need to have a structure to keep
   track of which psymtabs have which bincls in them.  This structure
   is used during readin to setup the list of dependencies within each
   partial symbol table. */

struct header_file_location
{
  char *name;			/* Name of header file */
  int instance;			/* See above */
  struct partial_symtab *pst;	/* Partial symtab that has the
				   BINCL/EINCL defs for this file */
};

/* The actual list and controling variables */
static struct header_file_location *bincl_list, *next_bincl;
static int bincls_allocated;

/* Local function prototypes */

static void
free_header_files PARAMS ((void));

static void
init_header_files PARAMS ((void));

static void
read_ofile_symtab PARAMS ((struct partial_symtab *));

static void
dbx_psymtab_to_symtab PARAMS ((struct partial_symtab *));

static void
dbx_psymtab_to_symtab_1 PARAMS ((struct partial_symtab *));

static void
read_dbx_dynamic_symtab PARAMS ((struct section_offsets *,
				 struct objfile *objfile));

static void
read_dbx_symtab PARAMS ((struct section_offsets *, struct objfile *,
			 CORE_ADDR, int));

static void
free_bincl_list PARAMS ((struct objfile *));

static struct partial_symtab *
find_corresponding_bincl_psymtab PARAMS ((char *, int));

static void
add_bincl_to_list PARAMS ((struct partial_symtab *, char *, int));

static void
init_bincl_list PARAMS ((int, struct objfile *));

static void
init_psymbol_list PARAMS ((struct objfile *));

static char *
dbx_next_symbol_text PARAMS ((void));

static void
fill_symbuf PARAMS ((bfd *));

static void
dbx_symfile_init PARAMS ((struct objfile *));

static void
dbx_new_init PARAMS ((struct objfile *));

static void
dbx_symfile_read PARAMS ((struct objfile *, struct section_offsets *, int));

static void
dbx_symfile_finish PARAMS ((struct objfile *));

static void
record_minimal_symbol PARAMS ((char *, CORE_ADDR, int, struct objfile *));

static void
add_new_header_file PARAMS ((char *, int));

static void
add_old_header_file PARAMS ((char *, int));

static void
add_this_object_header_file PARAMS ((int));

/* Free up old header file tables */

static void
free_header_files ()
{
  register int i;

  if (header_files != NULL)
    {
      for (i = 0; i < n_header_files; i++)
	{
	  free (header_files[i].name);
	}
      free ((PTR)header_files);
      header_files = NULL;
      n_header_files = 0;
    }
  if (this_object_header_files)
    {
      free ((PTR)this_object_header_files);
      this_object_header_files = NULL;
    }
  n_allocated_header_files = 0;
  n_allocated_this_object_header_files = 0;
}

/* Allocate new header file tables */

static void
init_header_files ()
{
  n_header_files = 0;
  n_allocated_header_files = 10;
  header_files = (struct header_file *)
    xmalloc (10 * sizeof (struct header_file));

  n_allocated_this_object_header_files = 10;
  this_object_header_files = (int *) xmalloc (10 * sizeof (int));
}

/* Add header file number I for this object file
   at the next successive FILENUM.  */

static void
add_this_object_header_file (i)
     int i;
{
  if (n_this_object_header_files == n_allocated_this_object_header_files)
    {
      n_allocated_this_object_header_files *= 2;
      this_object_header_files
	= (int *) xrealloc ((char *) this_object_header_files,
			    n_allocated_this_object_header_files * sizeof (int));
    }

  this_object_header_files[n_this_object_header_files++] = i;
}

/* Add to this file an "old" header file, one already seen in
   a previous object file.  NAME is the header file's name.
   INSTANCE is its instance code, to select among multiple
   symbol tables for the same header file.  */

static void
add_old_header_file (name, instance)
     char *name;
     int instance;
{
  register struct header_file *p = header_files;
  register int i;

  for (i = 0; i < n_header_files; i++)
    if (STREQ (p[i].name, name) && instance == p[i].instance)
      {
	add_this_object_header_file (i);
	return;
      }
  complain (&repeated_header_complaint, symnum);
  complain (&repeated_header_name_complaint, name);
}

/* Add to this file a "new" header file: definitions for its types follow.
   NAME is the header file's name.
   Most often this happens only once for each distinct header file,
   but not necessarily.  If it happens more than once, INSTANCE has
   a different value each time, and references to the header file
   use INSTANCE values to select among them.

   dbx output contains "begin" and "end" markers for each new header file,
   but at this level we just need to know which files there have been;
   so we record the file when its "begin" is seen and ignore the "end".  */

static void
add_new_header_file (name, instance)
     char *name;
     int instance;
{
  register int i;

  /* Make sure there is room for one more header file.  */

  if (n_header_files == n_allocated_header_files)
    {
      n_allocated_header_files *= 2;
      header_files = (struct header_file *)
	xrealloc ((char *) header_files,
		  (n_allocated_header_files * sizeof (struct header_file)));
    }

  /* Create an entry for this header file.  */

  i = n_header_files++;
  header_files[i].name = savestring (name, strlen(name));
  header_files[i].instance = instance;
  header_files[i].length = 10;
  header_files[i].vector
    = (struct type **) xmalloc (10 * sizeof (struct type *));
  memset (header_files[i].vector, 0, 10 * sizeof (struct type *));

  add_this_object_header_file (i);
}

#if 0
static struct type **
explicit_lookup_type (real_filenum, index)
     int real_filenum, index;
{
  register struct header_file *f = &header_files[real_filenum];

  if (index >= f->length)
    {
      f->length *= 2;
      f->vector = (struct type **)
	xrealloc (f->vector, f->length * sizeof (struct type *));
      memset (&f->vector[f->length / 2],
	     '\0', f->length * sizeof (struct type *) / 2);
    }
  return &f->vector[index];
}
#endif

static void
record_minimal_symbol (name, address, type, objfile)
     char *name;
     CORE_ADDR address;
     int type;
     struct objfile *objfile;
{
  enum minimal_symbol_type ms_type;

  switch (type)
    {
    case N_TEXT | N_EXT:  ms_type = mst_text; break;
    case N_DATA | N_EXT:  ms_type = mst_data; break;
    case N_BSS | N_EXT:   ms_type = mst_bss;  break;
    case N_ABS | N_EXT:   ms_type = mst_abs;  break;
#ifdef N_SETV
    case N_SETV | N_EXT:  ms_type = mst_data; break;
    case N_SETV:
      /* I don't think this type actually exists; since a N_SETV is the result
	 of going over many .o files, it doesn't make sense to have one
	 file local.  */
      ms_type = mst_file_data;
      break;
#endif
    case N_TEXT:
    case N_NBTEXT:
    case N_FN:
    case N_FN_SEQ:
      ms_type = mst_file_text;
      break;

    case N_DATA:
      ms_type = mst_file_data;

      /* Check for __DYNAMIC, which is used by Sun shared libraries. 
	 Record it as global even if it's local, not global, so
	 lookup_minimal_symbol can find it.  We don't check symbol_leading_char
	 because for SunOS4 it always is '_'.  */
      if (name[8] == 'C' && STREQ ("__DYNAMIC", name))
	ms_type = mst_data;

      /* Same with virtual function tables, both global and static.  */
      {
	char *tempstring = name;
	if (tempstring[0] == bfd_get_symbol_leading_char (objfile->obfd))
	  ++tempstring;
	if (VTBL_PREFIX_P ((tempstring)))
	  ms_type = mst_data;
      }
      break;

    case N_BSS:
      ms_type = mst_file_bss;
      break;

    default:      ms_type = mst_unknown; break;
  }

  if (ms_type == mst_file_text || ms_type == mst_text
      && address < lowest_text_address)
    lowest_text_address = address;

  prim_record_minimal_symbol
    (obsavestring (name, strlen (name), &objfile -> symbol_obstack),
     address,
     ms_type,
     objfile);
}

/* Scan and build partial symbols for a symbol file.
   We have been initialized by a call to dbx_symfile_init, which 
   put all the relevant info into a "struct dbx_symfile_info",
   hung off the objfile structure.

   SECTION_OFFSETS contains offsets relative to which the symbols in the
   various sections are (depending where the sections were actually loaded).
   MAINLINE is true if we are reading the main symbol
   table (as opposed to a shared lib or dynamically loaded file).  */

static void
dbx_symfile_read (objfile, section_offsets, mainline)
     struct objfile *objfile;
     struct section_offsets *section_offsets;
     int mainline;	/* FIXME comments above */
{
  bfd *sym_bfd;
  int val;
  struct cleanup *back_to;

  sym_bfd = objfile->obfd;
  val = bfd_seek (objfile->obfd, DBX_SYMTAB_OFFSET (objfile), SEEK_SET);
  if (val < 0)
    perror_with_name (objfile->name);

  /* If we are reinitializing, or if we have never loaded syms yet, init */
  if (mainline || objfile->global_psymbols.size == 0 || objfile->static_psymbols.size == 0)
    init_psymbol_list (objfile);

  symbol_size = DBX_SYMBOL_SIZE (objfile);
  symbol_table_offset = DBX_SYMTAB_OFFSET (objfile);

  pending_blocks = 0;
  back_to = make_cleanup (really_free_pendings, 0);

  init_minimal_symbol_collection ();
  make_cleanup (discard_minimal_symbols, 0);

  /* Now that the symbol table data of the executable file are all in core,
     process them and define symbols accordingly.  */

  read_dbx_symtab (section_offsets, objfile,
		   bfd_section_vma  (sym_bfd, DBX_TEXT_SECT (objfile)),
		   bfd_section_size (sym_bfd, DBX_TEXT_SECT (objfile)));

  /* Add the dynamic symbols if we are reading the main symbol table.  */

  if (mainline)
    read_dbx_dynamic_symtab (section_offsets, objfile);

  /* Install any minimal symbols that have been collected as the current
     minimal symbols for this objfile. */

  install_minimal_symbols (objfile);

  if (!have_partial_symbols ()) {
    wrap_here ("");
    printf_filtered ("(no debugging symbols found)...");
    wrap_here ("");
  }

  do_cleanups (back_to);
}

/* Initialize anything that needs initializing when a completely new
   symbol file is specified (not just adding some symbols from another
   file, e.g. a shared library).  */

static void
dbx_new_init (ignore)
     struct objfile *ignore;
{
  stabsread_new_init ();
  buildsym_new_init ();
  init_header_files ();
}


/* dbx_symfile_init ()
   is the dbx-specific initialization routine for reading symbols.
   It is passed a struct objfile which contains, among other things,
   the BFD for the file whose symbols are being read, and a slot for a pointer
   to "private data" which we fill with goodies.

   We read the string table into malloc'd space and stash a pointer to it.

   Since BFD doesn't know how to read debug symbols in a format-independent
   way (and may never do so...), we have to do it ourselves.  We will never
   be called unless this is an a.out (or very similar) file. 
   FIXME, there should be a cleaner peephole into the BFD environment here.  */

#define DBX_STRINGTAB_SIZE_SIZE sizeof(long)   /* FIXME */

static void
dbx_symfile_init (objfile)
     struct objfile *objfile;
{
  int val;
  bfd *sym_bfd = objfile->obfd;
  char *name = bfd_get_filename (sym_bfd);
  unsigned char size_temp[DBX_STRINGTAB_SIZE_SIZE];

  /* Allocate struct to keep track of the symfile */
  objfile->sym_stab_info = (PTR)
    xmmalloc (objfile -> md, sizeof (struct dbx_symfile_info));

  /* FIXME POKING INSIDE BFD DATA STRUCTURES */
#define	STRING_TABLE_OFFSET	(sym_bfd->origin + obj_str_filepos (sym_bfd))
#define	SYMBOL_TABLE_OFFSET	(sym_bfd->origin + obj_sym_filepos (sym_bfd))

  /* FIXME POKING INSIDE BFD DATA STRUCTURES */

  DBX_SYMFILE_INFO (objfile)->stab_section_info = NULL;
  DBX_TEXT_SECT (objfile) = bfd_get_section_by_name (sym_bfd, ".text");
  if (!DBX_TEXT_SECT (objfile))
    error ("Can't find .text section in symbol file");

  DBX_SYMBOL_SIZE (objfile) = obj_symbol_entry_size (sym_bfd);
  DBX_SYMCOUNT (objfile) = bfd_get_symcount (sym_bfd);
  DBX_SYMTAB_OFFSET (objfile) = SYMBOL_TABLE_OFFSET;

  /* Read the string table and stash it away in the psymbol_obstack.  It is
     only needed as long as we need to expand psymbols into full symbols,
     so when we blow away the psymbol the string table goes away as well.
     Note that gdb used to use the results of attempting to malloc the
     string table, based on the size it read, as a form of sanity check
     for botched byte swapping, on the theory that a byte swapped string
     table size would be so totally bogus that the malloc would fail.  Now
     that we put in on the psymbol_obstack, we can't do this since gdb gets
     a fatal error (out of virtual memory) if the size is bogus.  We can
     however at least check to see if the size is less than the size of
     the size field itself, or larger than the size of the entire file.
     Note that all valid string tables have a size greater than zero, since
     the bytes used to hold the size are included in the count. */

  if (STRING_TABLE_OFFSET == 0)
    {
      /* It appears that with the existing bfd code, STRING_TABLE_OFFSET
	 will never be zero, even when there is no string table.  This
	 would appear to be a bug in bfd. */
      DBX_STRINGTAB_SIZE (objfile) = 0;
      DBX_STRINGTAB (objfile) = NULL;
    }
  else
    {
      val = bfd_seek (sym_bfd, STRING_TABLE_OFFSET, SEEK_SET);
      if (val < 0)
	perror_with_name (name);
      
      memset ((PTR) size_temp, 0, sizeof (size_temp));
      val = bfd_read ((PTR) size_temp, sizeof (size_temp), 1, sym_bfd);
      if (val < 0)
	{
	  perror_with_name (name);
	}
      else if (val == 0)
	{
	  /* With the existing bfd code, STRING_TABLE_OFFSET will be set to
	     EOF if there is no string table, and attempting to read the size
	     from EOF will read zero bytes. */
	  DBX_STRINGTAB_SIZE (objfile) = 0;
	  DBX_STRINGTAB (objfile) = NULL;
	}
      else
	{
	  /* Read some data that would appear to be the string table size.
	     If there really is a string table, then it is probably the right
	     size.  Byteswap if necessary and validate the size.  Note that
	     the minimum is DBX_STRINGTAB_SIZE_SIZE.  If we just read some
	     random data that happened to be at STRING_TABLE_OFFSET, because
	     bfd can't tell us there is no string table, the sanity checks may
	     or may not catch this. */
	  DBX_STRINGTAB_SIZE (objfile) = bfd_h_get_32 (sym_bfd, size_temp);
	  
	  if (DBX_STRINGTAB_SIZE (objfile) < sizeof (size_temp)
	      || DBX_STRINGTAB_SIZE (objfile) > bfd_get_size (sym_bfd))
	    error ("ridiculous string table size (%d bytes).",
		   DBX_STRINGTAB_SIZE (objfile));
	  
	  DBX_STRINGTAB (objfile) =
	    (char *) obstack_alloc (&objfile -> psymbol_obstack,
				    DBX_STRINGTAB_SIZE (objfile));
	  
	  /* Now read in the string table in one big gulp.  */
	  
	  val = bfd_seek (sym_bfd, STRING_TABLE_OFFSET, SEEK_SET);
	  if (val < 0)
	    perror_with_name (name);
	  val = bfd_read (DBX_STRINGTAB (objfile), DBX_STRINGTAB_SIZE (objfile), 1,
			  sym_bfd);
	  if (val != DBX_STRINGTAB_SIZE (objfile))
	    perror_with_name (name);
	}
    }
}

/* Perform any local cleanups required when we are done with a particular
   objfile.  I.E, we are in the process of discarding all symbol information
   for an objfile, freeing up all memory held for it, and unlinking the
   objfile struct from the global list of known objfiles. */

static void
dbx_symfile_finish (objfile)
     struct objfile *objfile;
{
  if (objfile->sym_stab_info != NULL)
    {
      mfree (objfile -> md, objfile->sym_stab_info);
    }
  free_header_files ();
}


/* Buffer for reading the symbol table entries.  */
static struct internal_nlist symbuf[4096];
static int symbuf_idx;
static int symbuf_end;

/* Name of last function encountered.  Used in Solaris to approximate
   object file boundaries.  */
static char *last_function_name;

/* The address in memory of the string table of the object file we are
   reading (which might not be the "main" object file, but might be a
   shared library or some other dynamically loaded thing).  This is set
   by read_dbx_symtab when building psymtabs, and by read_ofile_symtab 
   when building symtabs, and is used only by next_symbol_text.  */
static char *stringtab_global;

/* Refill the symbol table input buffer
   and set the variables that control fetching entries from it.
   Reports an error if no data available.
   This function can read past the end of the symbol table
   (into the string table) but this does no harm.  */

static void
fill_symbuf (sym_bfd)
     bfd *sym_bfd;
{
  int nbytes = bfd_read ((PTR)symbuf, sizeof (symbuf), 1, sym_bfd);
  if (nbytes < 0)
    perror_with_name (bfd_get_filename (sym_bfd));
  else if (nbytes == 0)
    error ("Premature end of file reading symbol table");
  symbuf_end = nbytes / symbol_size;
  symbuf_idx = 0;
}

#define SWAP_SYMBOL(symp, abfd) \
  { \
    (symp)->n_strx = bfd_h_get_32(abfd,			\
				(unsigned char *)&(symp)->n_strx);	\
    (symp)->n_desc = bfd_h_get_16 (abfd,			\
				(unsigned char *)&(symp)->n_desc);  	\
    (symp)->n_value = bfd_h_get_32 (abfd,			\
				(unsigned char *)&(symp)->n_value); 	\
  }

/* Invariant: The symbol pointed to by symbuf_idx is the first one
   that hasn't been swapped.  Swap the symbol at the same time
   that symbuf_idx is incremented.  */

/* dbx allows the text of a symbol name to be continued into the
   next symbol name!  When such a continuation is encountered
   (a \ at the end of the text of a name)
   call this function to get the continuation.  */

static char *
dbx_next_symbol_text ()
{
  if (symbuf_idx == symbuf_end)
    fill_symbuf (symfile_bfd);
  symnum++;
  SWAP_SYMBOL(&symbuf[symbuf_idx], symfile_bfd);
  return symbuf[symbuf_idx++].n_strx + stringtab_global
	  + file_string_table_offset;
}

/* Initializes storage for all of the partial symbols that will be
   created by read_dbx_symtab and subsidiaries.  */

static void
init_psymbol_list (objfile)
     struct objfile *objfile;
{
  /* Free any previously allocated psymbol lists.  */
  if (objfile -> global_psymbols.list)
    mfree (objfile -> md, (PTR)objfile -> global_psymbols.list);
  if (objfile -> static_psymbols.list)
    mfree (objfile -> md, (PTR)objfile -> static_psymbols.list);

  /* Current best guess is that there are approximately a twentieth
     of the total symbols (in a debugging file) are global or static
     oriented symbols */
  objfile -> global_psymbols.size = DBX_SYMCOUNT (objfile) / 10;
  objfile -> static_psymbols.size = DBX_SYMCOUNT (objfile) / 10;
  objfile -> global_psymbols.next = objfile -> global_psymbols.list = (struct partial_symbol *)
    xmmalloc (objfile -> md, objfile -> global_psymbols.size * sizeof (struct partial_symbol));
  objfile -> static_psymbols.next = objfile -> static_psymbols.list = (struct partial_symbol *)
    xmmalloc (objfile -> md, objfile -> static_psymbols.size * sizeof (struct partial_symbol));
}

/* Initialize the list of bincls to contain none and have some
   allocated.  */

static void
init_bincl_list (number, objfile)
     int number;
     struct objfile *objfile;
{
  bincls_allocated = number;
  next_bincl = bincl_list = (struct header_file_location *)
    xmmalloc (objfile -> md, bincls_allocated * sizeof(struct header_file_location));
}

/* Add a bincl to the list.  */

static void
add_bincl_to_list (pst, name, instance)
     struct partial_symtab *pst;
     char *name;
     int instance;
{
  if (next_bincl >= bincl_list + bincls_allocated)
    {
      int offset = next_bincl - bincl_list;
      bincls_allocated *= 2;
      bincl_list = (struct header_file_location *)
	xmrealloc (pst->objfile->md, (char *)bincl_list,
		  bincls_allocated * sizeof (struct header_file_location));
      next_bincl = bincl_list + offset;
    }
  next_bincl->pst = pst;
  next_bincl->instance = instance;
  next_bincl++->name = name;
}

/* Given a name, value pair, find the corresponding
   bincl in the list.  Return the partial symtab associated
   with that header_file_location.  */

static struct partial_symtab *
find_corresponding_bincl_psymtab (name, instance)
     char *name;
     int instance;
{
  struct header_file_location *bincl;

  for (bincl = bincl_list; bincl < next_bincl; bincl++)
    if (bincl->instance == instance
	&& STREQ (name, bincl->name))
      return bincl->pst;

  return (struct partial_symtab *) 0;
}

/* Free the storage allocated for the bincl list.  */

static void
free_bincl_list (objfile)
     struct objfile *objfile;
{
  mfree (objfile -> md, (PTR)bincl_list);
  bincls_allocated = 0;
}

/* Scan a SunOs dynamic symbol table for symbols of interest and
   add them to the minimal symbol table.  */

static void
read_dbx_dynamic_symtab (section_offsets, objfile)
     struct section_offsets *section_offsets;
     struct objfile *objfile;
{
  bfd *abfd = objfile->obfd;
  int counter;
  bfd_size_type dynsym_count = 0;
  struct external_nlist *dynsyms = NULL;
  char *dynstrs = NULL;
  bfd_size_type dynstr_size;
  struct external_nlist *ext_symptr;
  bfd_byte *ext_relptr;
  bfd_size_type dynrel_count = 0;
  PTR dynrels = NULL;
  CORE_ADDR sym_value;
  bfd_vma strx;
  char *namestring;

  /* Check that the symbol file has dynamic symbols that we know about.
     bfd_arch_unknown can happen if we are reading a sun3 symbol file
     on a sun4 host (and vice versa) and bfd is not configured
     --with-target=all.  This would trigger an assertion in bfd/sunos.c,
     so we ignore the dynamic symbols in this case.  */
  if (bfd_get_flavour (abfd) != bfd_target_aout_flavour
      || (bfd_get_file_flags (abfd) & DYNAMIC) == 0
      || bfd_get_arch (abfd) == bfd_arch_unknown
      || aout_backend_info (abfd)->read_dynamic_symbols == NULL)
    return;

  dynsym_count = ((*aout_backend_info (abfd)->read_dynamic_symbols)
		  (abfd, &dynsyms, &dynstrs, &dynstr_size));
  if (dynsym_count == (bfd_size_type) -1)
    return;

  /* Enter dynamic symbols into the minimal symbol table
     if this is a stripped executable.  */
  if (bfd_get_symcount (abfd) <= 0)
    {
      ext_symptr = dynsyms;
      for (counter = 0; counter < dynsym_count; counter++, ext_symptr++)
	{
	  int type = bfd_h_get_8 (abfd, ext_symptr->e_type);

	  switch (type)
	    {
	    case N_TEXT | N_EXT:
	      sym_value = bfd_h_get_32 (abfd, ext_symptr->e_value)
			  + ANOFFSET (section_offsets, SECT_OFF_TEXT);
	      break;

	    case N_DATA:
	    case N_DATA | N_EXT:
	      sym_value = bfd_h_get_32 (abfd, ext_symptr->e_value)
			  + ANOFFSET (section_offsets, SECT_OFF_DATA);
	      break;

	    case N_BSS:
	    case N_BSS | N_EXT:
	      sym_value = bfd_h_get_32 (abfd, ext_symptr->e_value)
			  + ANOFFSET (section_offsets, SECT_OFF_BSS);
	      break;

	    default:
	      continue;
	    }

	  strx = bfd_h_get_32 (abfd, ext_symptr->e_strx);
	  if (strx >= dynstr_size)
	    {
	      complain (&string_table_offset_complaint, counter);
	      namestring = "<bad dynamic string table offset>";
	    }
	  else
	    namestring = strx + dynstrs;
	  record_minimal_symbol (namestring, sym_value, type, objfile);
	}
    }

  /* Symbols from shared libraries have a dynamic relocation entry
     that points to the associated slot in the procedure linkage table.
     We make a mininal symbol table entry with type mst_solib_trampoline
     at the address in the procedure linkage table.  */
  if (aout_backend_info (abfd)->read_dynamic_relocs == NULL)
    return;

  dynrel_count = ((*aout_backend_info (abfd)->read_dynamic_relocs)
		  (abfd, &dynrels));
  if (dynrel_count == (bfd_size_type) -1)
    return;

  for (counter = 0, ext_relptr = (bfd_byte *) dynrels;
       counter < dynrel_count;
       counter++, ext_relptr += obj_reloc_entry_size (abfd))
    {
      int r_index;

      if (bfd_get_arch (abfd) == bfd_arch_sparc)
	{
	  struct reloc_ext_external *rptr =
	    (struct reloc_ext_external *) ext_relptr;
	  int r_type;

	  r_type = (rptr->r_type[0] & RELOC_EXT_BITS_TYPE_BIG)
		    >> RELOC_EXT_BITS_TYPE_SH_BIG;

	  if (r_type != RELOC_JMP_SLOT)
	    continue;

	  r_index = (rptr->r_index[0] << 16)
		    | (rptr->r_index[1] << 8)
		    | rptr->r_index[2];

	  sym_value = bfd_h_get_32 (abfd, rptr->r_address);
	}
      else if (bfd_get_arch (abfd) == bfd_arch_m68k)
	{
	  struct reloc_std_external *rptr =
	    (struct reloc_std_external *) ext_relptr;

	  if ((rptr->r_type[0] & RELOC_STD_BITS_JMPTABLE_BIG) == 0)
	    continue;

	  r_index = (rptr->r_index[0] << 16)
		    | (rptr->r_index[1] << 8)
		    | rptr->r_index[2];

	  /* Adjust address in procedure linkage table to point to
	     the start of the bsr instruction.  */
	  sym_value = bfd_h_get_32 (abfd, rptr->r_address) - 2;
	}
      else
	{
	  continue;
	}

      if (r_index >= dynsym_count)
	continue;
      ext_symptr = dynsyms + r_index;
      if (bfd_h_get_8 (abfd, ext_symptr->e_type) != N_EXT)
	continue;

      strx = bfd_h_get_32 (abfd, ext_symptr->e_strx);
      if (strx >= dynstr_size)
	{
	  complain (&string_table_offset_complaint, r_index);
	  namestring = "<bad dynamic string table offset>";
	}
      else
	namestring = strx + dynstrs;

      prim_record_minimal_symbol (obsavestring (namestring,
						strlen (namestring),
						&objfile -> symbol_obstack),
				  sym_value,
				  mst_solib_trampoline,
				  objfile);
    }
}

/* Given pointers to an a.out symbol table in core containing dbx
   style data, setup partial_symtab's describing each source file for
   which debugging information is available.
   SYMFILE_NAME is the name of the file we are reading from
   and SECTION_OFFSETS is the set of offsets for the various sections
   of the file (a set of zeros if the mainline program).  */

static void
read_dbx_symtab (section_offsets, objfile, text_addr, text_size)
     struct section_offsets *section_offsets;
     struct objfile *objfile;
     CORE_ADDR text_addr;
     int text_size;
{
  register struct internal_nlist *bufp = 0;	/* =0 avoids gcc -Wall glitch */
  register char *namestring;
  int nsl;
  int past_first_source_file = 0;
  CORE_ADDR last_o_file_start = 0;
  struct cleanup *back_to;
  bfd *abfd;

  /* End of the text segment of the executable file.  */
  CORE_ADDR end_of_text_addr;

  /* Current partial symtab */
  struct partial_symtab *pst;

  /* List of current psymtab's include files */
  char **psymtab_include_list;
  int includes_allocated;
  int includes_used;

  /* Index within current psymtab dependency list */
  struct partial_symtab **dependency_list;
  int dependencies_used, dependencies_allocated;

  /* FIXME.  We probably want to change stringtab_global rather than add this
     while processing every symbol entry.  FIXME.  */
  file_string_table_offset = 0;
  next_file_string_table_offset = 0;

  stringtab_global = DBX_STRINGTAB (objfile);
  
  pst = (struct partial_symtab *) 0;

  includes_allocated = 30;
  includes_used = 0;
  psymtab_include_list = (char **) alloca (includes_allocated *
					   sizeof (char *));

  dependencies_allocated = 30;
  dependencies_used = 0;
  dependency_list =
    (struct partial_symtab **) alloca (dependencies_allocated *
				       sizeof (struct partial_symtab *));

  /* Init bincl list */
  init_bincl_list (20, objfile);
  back_to = make_cleanup (free_bincl_list, objfile);

  last_source_file = NULL;

  lowest_text_address = (CORE_ADDR)-1;

  symfile_bfd = objfile->obfd;	/* For next_text_symbol */
  abfd = objfile->obfd;
  symbuf_end = symbuf_idx = 0;
  next_symbol_text_func = dbx_next_symbol_text;

  for (symnum = 0; symnum < DBX_SYMCOUNT (objfile); symnum++)
    {
      /* Get the symbol for this run and pull out some info */
      QUIT;	/* allow this to be interruptable */
      if (symbuf_idx == symbuf_end)
	fill_symbuf (abfd);
      bufp = &symbuf[symbuf_idx++];

      /*
       * Special case to speed up readin.
       */
      if (bufp->n_type == (unsigned char)N_SLINE) continue;

      SWAP_SYMBOL (bufp, abfd);

      /* Ok.  There is a lot of code duplicated in the rest of this
         switch statement (for efficiency reasons).  Since I don't
         like duplicating code, I will do my penance here, and
         describe the code which is duplicated:

	 *) The assignment to namestring.
	 *) The call to strchr.
	 *) The addition of a partial symbol the the two partial
	    symbol lists.  This last is a large section of code, so
	    I've imbedded it in the following macro.
	 */
      
/* Set namestring based on bufp.  If the string table index is invalid, 
   give a fake name, and print a single error message per symbol file read,
   rather than abort the symbol reading or flood the user with messages.  */

/*FIXME: Too many adds and indirections in here for the inner loop.  */
#define SET_NAMESTRING()\
  if (((unsigned)bufp->n_strx + file_string_table_offset) >=		\
      DBX_STRINGTAB_SIZE (objfile)) {					\
    complain (&string_table_offset_complaint, symnum);			\
    namestring = "<bad string table offset>";				\
  } else								\
    namestring = bufp->n_strx + file_string_table_offset +		\
		 DBX_STRINGTAB (objfile)

#define CUR_SYMBOL_TYPE bufp->n_type
#define CUR_SYMBOL_VALUE bufp->n_value
#define DBXREAD_ONLY
#define START_PSYMTAB(ofile,secoff,fname,low,symoff,global_syms,static_syms)\
  start_psymtab(ofile, secoff, fname, low, symoff, global_syms, static_syms)
#define END_PSYMTAB(pst,ilist,ninc,c_off,c_text,dep_list,n_deps)\
  end_psymtab(pst,ilist,ninc,c_off,c_text,dep_list,n_deps)

#include "partial-stab.h"
    }

  /* If there's stuff to be cleaned up, clean it up.  */
  if (DBX_SYMCOUNT (objfile) > 0			/* We have some syms */
/*FIXME, does this have a bug at start address 0? */
      && last_o_file_start
      && objfile -> ei.entry_point < bufp->n_value
      && objfile -> ei.entry_point >= last_o_file_start)
    {
      objfile -> ei.entry_file_lowpc = last_o_file_start;
      objfile -> ei.entry_file_highpc = bufp->n_value;
    }

  if (pst)
    {
      end_psymtab (pst, psymtab_include_list, includes_used,
		   symnum * symbol_size,
		   (lowest_text_address == (CORE_ADDR)-1
		    ? text_addr : lowest_text_address)
		   + text_size,
		   dependency_list, dependencies_used);
    }

  do_cleanups (back_to);
}

/* Allocate and partially fill a partial symtab.  It will be
   completely filled at the end of the symbol list.

   SYMFILE_NAME is the name of the symbol-file we are reading from, and ADDR
   is the address relative to which its symbols are (incremental) or 0
   (normal). */


struct partial_symtab *
start_psymtab (objfile, section_offsets,
	       filename, textlow, ldsymoff, global_syms, static_syms)
     struct objfile *objfile;
     struct section_offsets *section_offsets;
     char *filename;
     CORE_ADDR textlow;
     int ldsymoff;
     struct partial_symbol *global_syms;
     struct partial_symbol *static_syms;
{
  struct partial_symtab *result =
      start_psymtab_common(objfile, section_offsets,
			   filename, textlow, global_syms, static_syms);

  result->read_symtab_private = (char *)
    obstack_alloc (&objfile -> psymbol_obstack, sizeof (struct symloc));
  LDSYMOFF(result) = ldsymoff;
  result->read_symtab = dbx_psymtab_to_symtab;
  SYMBOL_SIZE(result) = symbol_size;
  SYMBOL_OFFSET(result) = symbol_table_offset;
  STRING_OFFSET(result) = string_table_offset;
  FILE_STRING_OFFSET(result) = file_string_table_offset;

  /* If we're handling an ELF file, drag some section-relocation info
     for this source file out of the ELF symbol table, to compensate for
     Sun brain death.  This replaces the section_offsets in this psymtab,
     if successful.  */
  elfstab_offset_sections (objfile, result);

  /* Deduce the source language from the filename for this psymtab. */
  psymtab_language = deduce_language_from_filename (filename);

  return result;
}

/* Close off the current usage of PST.  
   Returns PST or NULL if the partial symtab was empty and thrown away.

   FIXME:  List variables and peculiarities of same.  */

struct partial_symtab *
end_psymtab (pst, include_list, num_includes, capping_symbol_offset,
	     capping_text, dependency_list, number_dependencies)
     struct partial_symtab *pst;
     char **include_list;
     int num_includes;
     int capping_symbol_offset;
     CORE_ADDR capping_text;
     struct partial_symtab **dependency_list;
     int number_dependencies;
{
  int i;
  struct partial_symtab *p1;
  struct objfile *objfile = pst -> objfile;

  if (capping_symbol_offset != -1)
      LDSYMLEN(pst) = capping_symbol_offset - LDSYMOFF(pst);
  pst->texthigh = capping_text;

#ifdef N_SO_ADDRESS_MAYBE_MISSING
  /* Under Solaris, the N_SO symbols always have a value of 0,
     instead of the usual address of the .o file.  Therefore,
     we have to do some tricks to fill in texthigh and textlow.
     The first trick is in partial-stab.h: if we see a static
     or global function, and the textlow for the current pst
     is still 0, then we use that function's address for 
     the textlow of the pst.

     Now, to fill in texthigh, we remember the last function seen
     in the .o file (also in partial-stab.h).  Also, there's a hack in
     bfd/elf.c and gdb/elfread.c to pass the ELF st_size field
     to here via the misc_info field.  Therefore, we can fill in
     a reliable texthigh by taking the address plus size of the
     last function in the file.

     Unfortunately, that does not cover the case where the last function
     in the file is static.  See the paragraph below for more comments
     on this situation.

     Finally, if we have a valid textlow for the current file, we run
     down the partial_symtab_list filling in previous texthighs that
     are still unknown.  */

  if (pst->texthigh == 0 && last_function_name) {
    char *p;
    int n;
    struct minimal_symbol *minsym;

    p = strchr (last_function_name, ':');
    if (p == NULL)
      p = last_function_name;
    n = p - last_function_name;
    p = alloca (n + 1);
    strncpy (p, last_function_name, n);
    p[n] = 0;
    
    minsym = lookup_minimal_symbol (p, objfile);

    if (minsym) {
      pst->texthigh = SYMBOL_VALUE_ADDRESS (minsym) +
	(long) MSYMBOL_INFO (minsym);
    } else {
      /* This file ends with a static function, and it's
	 difficult to imagine how hard it would be to track down
	 the elf symbol.  Luckily, most of the time no one will notice,
	 since the next file will likely be compiled with -g, so
	 the code below will copy the first fuction's start address 
	 back to our texthigh variable.  (Also, if this file is the
	 last one in a dynamically linked program, texthigh already
	 has the right value.)  If the next file isn't compiled
	 with -g, then the last function in this file winds up owning
	 all of the text space up to the next -g file, or the end (minus
	 shared libraries).  This only matters for single stepping,
	 and even then it will still work, except that it will single
	 step through all of the covered functions, instead of setting
	 breakpoints around them as it usualy does.  This makes it
	 pretty slow, but at least it doesn't fail.

	 We can fix this with a fairly big change to bfd, but we need
	 to coordinate better with Cygnus if we want to do that.  FIXME.  */
    }
    last_function_name = NULL;
  }

  /* this test will be true if the last .o file is only data */
  if (pst->textlow == 0)
    /* This loses if the text section really starts at address zero
       (generally true when we are debugging a .o file, for example).
       That is why this whole thing is inside N_SO_ADDRESS_MAYBE_MISSING.  */
    pst->textlow = pst->texthigh;

  /* If we know our own starting text address, then walk through all other
     psymtabs for this objfile, and if any didn't know their ending text
     address, set it to our starting address.  Take care to not set our
     own ending address to our starting address, nor to set addresses on
     `dependency' files that have both textlow and texthigh zero.  */
  if (pst->textlow) {
    ALL_OBJFILE_PSYMTABS (objfile, p1) {
      if (p1->texthigh == 0  && p1->textlow != 0 && p1 != pst) {
	p1->texthigh = pst->textlow;
	/* if this file has only data, then make textlow match texthigh */
	if (p1->textlow == 0)
	  p1->textlow = p1->texthigh;
      }
    }
  }

  /* End of kludge for patching Solaris textlow and texthigh.  */
#endif /* N_SO_ADDRESS_MAYBE_MISSING.  */

  pst->n_global_syms =
    objfile->global_psymbols.next - (objfile->global_psymbols.list + pst->globals_offset);
  pst->n_static_syms =
    objfile->static_psymbols.next - (objfile->static_psymbols.list + pst->statics_offset);

  pst->number_of_dependencies = number_dependencies;
  if (number_dependencies)
    {
      pst->dependencies = (struct partial_symtab **)
	obstack_alloc (&objfile->psymbol_obstack,
		       number_dependencies * sizeof (struct partial_symtab *));
      memcpy (pst->dependencies, dependency_list,
	     number_dependencies * sizeof (struct partial_symtab *));
    }
  else
    pst->dependencies = 0;

  for (i = 0; i < num_includes; i++)
    {
      struct partial_symtab *subpst =
	allocate_psymtab (include_list[i], objfile);

      subpst->section_offsets = pst->section_offsets;
      subpst->read_symtab_private =
	  (char *) obstack_alloc (&objfile->psymbol_obstack,
				  sizeof (struct symloc));
      LDSYMOFF(subpst) =
	LDSYMLEN(subpst) =
	  subpst->textlow =
	    subpst->texthigh = 0;

      /* We could save slight bits of space by only making one of these,
	 shared by the entire set of include files.  FIXME-someday.  */
      subpst->dependencies = (struct partial_symtab **)
	obstack_alloc (&objfile->psymbol_obstack,
		       sizeof (struct partial_symtab *));
      subpst->dependencies[0] = pst;
      subpst->number_of_dependencies = 1;

      subpst->globals_offset =
	subpst->n_global_syms =
	  subpst->statics_offset =
	    subpst->n_static_syms = 0;

      subpst->readin = 0;
      subpst->symtab = 0;
      subpst->read_symtab = pst->read_symtab;
    }

  sort_pst_symbols (pst);

  /* If there is already a psymtab or symtab for a file of this name, remove it.
     (If there is a symtab, more drastic things also happen.)
     This happens in VxWorks.  */
  free_named_symtabs (pst->filename);

  if (num_includes == 0
      && number_dependencies == 0
      && pst->n_global_syms == 0
      && pst->n_static_syms == 0)
    {
      /* Throw away this psymtab, it's empty.  We can't deallocate it, since
	 it is on the obstack, but we can forget to chain it on the list.  */
      /* Empty psymtabs happen as a result of header files which don't have
	 any symbols in them.  There can be a lot of them.  But this check
	 is wrong, in that a psymtab with N_SLINE entries but nothing else
	 is not empty, but we don't realize that.  Fixing that without slowing
	 things down might be tricky.  */
      struct partial_symtab *prev_pst;

      /* First, snip it out of the psymtab chain */

      if (pst->objfile->psymtabs == pst)
	pst->objfile->psymtabs = pst->next;
      else
	for (prev_pst = pst->objfile->psymtabs; prev_pst; prev_pst = pst->next)
	  if (prev_pst->next == pst)
	    prev_pst->next = pst->next;

      /* Next, put it on a free list for recycling */

      pst->next = pst->objfile->free_psymtabs;
      pst->objfile->free_psymtabs = pst;

      /* Indicate that psymtab was thrown away.  */
      pst = (struct partial_symtab *)NULL;
    }
  return pst;
}

static void
dbx_psymtab_to_symtab_1 (pst)
     struct partial_symtab *pst;
{
  struct cleanup *old_chain;
  int i;
  
  if (!pst)
    return;

  if (pst->readin)
    {
      fprintf_unfiltered (gdb_stderr, "Psymtab for %s already read in.  Shouldn't happen.\n",
	       pst->filename);
      return;
    }

  /* Read in all partial symtabs on which this one is dependent */
  for (i = 0; i < pst->number_of_dependencies; i++)
    if (!pst->dependencies[i]->readin)
      {
	/* Inform about additional files that need to be read in.  */
	if (info_verbose)
	  {
	    fputs_filtered (" ", gdb_stdout);
	    wrap_here ("");
	    fputs_filtered ("and ", gdb_stdout);
	    wrap_here ("");
	    printf_filtered ("%s...", pst->dependencies[i]->filename);
	    wrap_here ("");		/* Flush output */
	    gdb_flush (gdb_stdout);
	  }
	dbx_psymtab_to_symtab_1 (pst->dependencies[i]);
      }

  if (LDSYMLEN(pst))		/* Otherwise it's a dummy */
    {
      /* Init stuff necessary for reading in symbols */
      stabsread_init ();
      buildsym_init ();
      old_chain = make_cleanup (really_free_pendings, 0);
      file_string_table_offset = FILE_STRING_OFFSET (pst);
      symbol_size = SYMBOL_SIZE (pst);

      /* Read in this file's symbols */
      bfd_seek (pst->objfile->obfd, SYMBOL_OFFSET (pst), SEEK_SET);
      read_ofile_symtab (pst);
      sort_symtab_syms (pst->symtab);

      do_cleanups (old_chain);
    }

  pst->readin = 1;
}

/* Read in all of the symbols for a given psymtab for real.
   Be verbose about it if the user wants that.  */

static void
dbx_psymtab_to_symtab (pst)
     struct partial_symtab *pst;
{
  bfd *sym_bfd;

  if (!pst)
    return;

  if (pst->readin)
    {
      fprintf_unfiltered (gdb_stderr, "Psymtab for %s already read in.  Shouldn't happen.\n",
	       pst->filename);
      return;
    }

  if (LDSYMLEN(pst) || pst->number_of_dependencies)
    {
      /* Print the message now, before reading the string table,
	 to avoid disconcerting pauses.  */
      if (info_verbose)
	{
	  printf_filtered ("Reading in symbols for %s...", pst->filename);
	  gdb_flush (gdb_stdout);
	}

      sym_bfd = pst->objfile->obfd;

      next_symbol_text_func = dbx_next_symbol_text;

      dbx_psymtab_to_symtab_1 (pst);

      /* Match with global symbols.  This only needs to be done once,
         after all of the symtabs and dependencies have been read in.   */
      scan_file_globals (pst->objfile);

      /* Finish up the debug error message.  */
      if (info_verbose)
	printf_filtered ("done.\n");
    }
}

/* Read in a defined section of a specific object file's symbols. */
  
static void
read_ofile_symtab (pst)
     struct partial_symtab *pst;
{
  register char *namestring;
  register struct internal_nlist *bufp;
  unsigned char type;
  unsigned max_symnum;
  register bfd *abfd;
  struct objfile *objfile;
  int sym_offset;		/* Offset to start of symbols to read */
  int sym_size;			/* Size of symbols to read */
  CORE_ADDR text_offset;	/* Start of text segment for symbols */
  int text_size;		/* Size of text segment for symbols */
  struct section_offsets *section_offsets;

  objfile = pst->objfile;
  sym_offset = LDSYMOFF(pst);
  sym_size = LDSYMLEN(pst);
  text_offset = pst->textlow;
  text_size = pst->texthigh - pst->textlow;
  section_offsets = pst->section_offsets;

  current_objfile = objfile;
  subfile_stack = NULL;

  stringtab_global = DBX_STRINGTAB (objfile);
  last_source_file = NULL;

  abfd = objfile->obfd;
  symfile_bfd = objfile->obfd;	/* Implicit param to next_text_symbol */
  symbuf_end = symbuf_idx = 0;

  /* It is necessary to actually read one symbol *before* the start
     of this symtab's symbols, because the GCC_COMPILED_FLAG_SYMBOL
     occurs before the N_SO symbol.

     Detecting this in read_dbx_symtab
     would slow down initial readin, so we look for it here instead.  */
  if (!processing_acc_compilation && sym_offset >= (int)symbol_size)
    {
      bfd_seek (symfile_bfd, sym_offset - symbol_size, SEEK_CUR);
      fill_symbuf (abfd);
      bufp = &symbuf[symbuf_idx++];
      SWAP_SYMBOL (bufp, abfd);

      SET_NAMESTRING ();

      processing_gcc_compilation = 0;
      if (bufp->n_type == N_TEXT)
	{
	  if (STREQ (namestring, GCC_COMPILED_FLAG_SYMBOL))
	    processing_gcc_compilation = 1;
	  else if (STREQ (namestring, GCC2_COMPILED_FLAG_SYMBOL))
	    processing_gcc_compilation = 2;
	}

      /* Try to select a C++ demangling based on the compilation unit
	 producer. */

      if (processing_gcc_compilation)
	{
	  if (AUTO_DEMANGLING)
	    {
	      set_demangling_style (GNU_DEMANGLING_STYLE_STRING);
	    }
	}
    }
  else
    {
      /* The N_SO starting this symtab is the first symbol, so we
	 better not check the symbol before it.  I'm not this can
	 happen, but it doesn't hurt to check for it.  */
      bfd_seek (symfile_bfd, sym_offset, SEEK_CUR);
      processing_gcc_compilation = 0;
    }

  if (symbuf_idx == symbuf_end)
    fill_symbuf (abfd);
  bufp = &symbuf[symbuf_idx];
  if (bufp->n_type != (unsigned char)N_SO)
    error("First symbol in segment of executable not a source symbol");

  max_symnum = sym_size / symbol_size;

  for (symnum = 0;
       symnum < max_symnum;
       symnum++)
    {
      QUIT;			/* Allow this to be interruptable */
      if (symbuf_idx == symbuf_end)
	fill_symbuf(abfd);
      bufp = &symbuf[symbuf_idx++];
      SWAP_SYMBOL (bufp, abfd);

      type = bufp->n_type;

      SET_NAMESTRING ();

      if (type & N_STAB) {
	  process_one_symbol (type, bufp->n_desc, bufp->n_value,
			      namestring, section_offsets, objfile);
      }
      /* We skip checking for a new .o or -l file; that should never
         happen in this routine. */
      else if (type == N_TEXT)
	{
	  /* I don't think this code will ever be executed, because
	     the GCC_COMPILED_FLAG_SYMBOL usually is right before
	     the N_SO symbol which starts this source file.
	     However, there is no reason not to accept
	     the GCC_COMPILED_FLAG_SYMBOL anywhere.  */

	  if (STREQ (namestring, GCC_COMPILED_FLAG_SYMBOL))
	    processing_gcc_compilation = 1;
	  else if (STREQ (namestring, GCC2_COMPILED_FLAG_SYMBOL))
	    processing_gcc_compilation = 2;

	  if (AUTO_DEMANGLING)
	    {
	      set_demangling_style (GNU_DEMANGLING_STYLE_STRING);
	    }
	}
      else if (type & N_EXT || type == (unsigned char)N_TEXT
	       || type == (unsigned char)N_NBTEXT
	       ) {
	  /* Global symbol: see if we came across a dbx defintion for
	     a corresponding symbol.  If so, store the value.  Remove
	     syms from the chain when their values are stored, but
	     search the whole chain, as there may be several syms from
	     different files with the same name. */
	  /* This is probably not true.  Since the files will be read
	     in one at a time, each reference to a global symbol will
	     be satisfied in each file as it appears. So we skip this
	     section. */
	  ;
        }
    }

  current_objfile = NULL;

  /* In a Solaris elf file, this variable, which comes from the
     value of the N_SO symbol, will still be 0.  Luckily, text_offset,
     which comes from pst->textlow is correct. */
  if (last_source_start_addr == 0)
    last_source_start_addr = text_offset;

  pst->symtab = end_symtab (text_offset + text_size, 0, 0, objfile,
			    SECT_OFF_TEXT);
  end_stabs ();
}


/* This handles a single symbol from the symbol-file, building symbols
   into a GDB symtab.  It takes these arguments and an implicit argument.

   TYPE is the type field of the ".stab" symbol entry.
   DESC is the desc field of the ".stab" entry.
   VALU is the value field of the ".stab" entry.
   NAME is the symbol name, in our address space.
   SECTION_OFFSETS is a set of amounts by which the sections of this object
          file were relocated when it was loaded into memory.
          All symbols that refer
	  to memory locations need to be offset by these amounts.
   OBJFILE is the object file from which we are reading symbols.
 	       It is used in end_symtab.  */

void
process_one_symbol (type, desc, valu, name, section_offsets, objfile)
     int type, desc;
     CORE_ADDR valu;
     char *name;
     struct section_offsets *section_offsets;
     struct objfile *objfile;
{
#ifdef SUN_FIXED_LBRAC_BUG
  /* If SUN_FIXED_LBRAC_BUG is defined, then it tells us whether we need
     to correct the address of N_LBRAC's.  If it is not defined, then
     we never need to correct the addresses.  */

  /* This records the last pc address we've seen.  We depend on there being
     an SLINE or FUN or SO before the first LBRAC, since the variable does
     not get reset in between reads of different symbol files.  */
  static CORE_ADDR last_pc_address;
#endif

  register struct context_stack *new;
  /* This remembers the address of the start of a function.  It is used
     because in Solaris 2, N_LBRAC, N_RBRAC, and N_SLINE entries are
     relative to the current function's start address.  On systems
     other than Solaris 2, this just holds the SECT_OFF_TEXT value, and is
     used to relocate these symbol types rather than SECTION_OFFSETS.  */
  static CORE_ADDR function_start_offset;

  /* If this is nonzero, N_LBRAC, N_RBRAC, and N_SLINE entries are relative
     to the function start address.  */
  int block_address_function_relative;

  /* If this is nonzero, we've seen a non-gcc N_OPT symbol for this source
     file.  Used to detect the SunPRO solaris compiler.  */
  static int n_opt_found;

  /* The stab type used for the definition of the last function.
     N_STSYM or N_GSYM for SunOS4 acc; N_FUN for other compilers.  */
  static int function_stab_type = 0;

  /* This is true for Solaris (and all other systems which put stabs
     in sections, hopefully, since it would be silly to do things
     differently from Solaris), and false for SunOS4 and other a.out
     file formats.  */
  block_address_function_relative =
    ((0 == strncmp (bfd_get_target (objfile->obfd), "elf", 3))
     || (0 == strncmp (bfd_get_target (objfile->obfd), "som", 3))
     || (0 == strncmp (bfd_get_target (objfile->obfd), "coff", 4)));

  if (!block_address_function_relative)
    /* N_LBRAC, N_RBRAC and N_SLINE entries are not relative to the
       function start address, so just use the text offset.  */
    function_start_offset = ANOFFSET (section_offsets, SECT_OFF_TEXT);

  /* Something is wrong if we see real data before
     seeing a source file name.  */

  if (last_source_file == NULL && type != (unsigned char)N_SO)
    {
      /* Ignore any symbols which appear before an N_SO symbol.  Currently
	 no one puts symbols there, but we should deal gracefully with the
	 case.  A complain()t might be in order (if !IGNORE_SYMBOL (type)),
	 but this should not be an error ().  */
      return;
    }

  switch (type)
    {
    case N_FUN:
    case N_FNAME:
      /* Relocate for dynamic loading */
      valu += ANOFFSET (section_offsets, SECT_OFF_TEXT);
      goto define_a_symbol;

    case N_LBRAC:
      /* This "symbol" just indicates the start of an inner lexical
	 context within a function.  */

#if defined(BLOCK_ADDRESS_ABSOLUTE)
      /* Relocate for dynamic loading (?).  */
      valu += function_start_offset;
#else
      if (block_address_function_relative)
	/* Relocate for Sun ELF acc fn-relative syms.  */
	valu += function_start_offset;
      else
	/* On most machines, the block addresses are relative to the
	   N_SO, the linker did not relocate them (sigh).  */
	valu += last_source_start_addr;
#endif

#ifdef SUN_FIXED_LBRAC_BUG
      if (!SUN_FIXED_LBRAC_BUG && valu < last_pc_address) {
	/* Patch current LBRAC pc value to match last handy pc value */
 	complain (&lbrac_complaint);
	valu = last_pc_address;
      }
#endif
      new = push_context (desc, valu);
      break;

    case N_RBRAC:
      /* This "symbol" just indicates the end of an inner lexical
	 context that was started with N_LBRAC.  */

#if defined(BLOCK_ADDRESS_ABSOLUTE)
      /* Relocate for dynamic loading (?).  */
      valu += function_start_offset;
#else
      if (block_address_function_relative)
	/* Relocate for Sun ELF acc fn-relative syms.  */
	valu += function_start_offset;
      else
	/* On most machines, the block addresses are relative to the
	   N_SO, the linker did not relocate them (sigh).  */
	valu += last_source_start_addr;
#endif

      new = pop_context();
      if (desc != new->depth)
	complain (&lbrac_mismatch_complaint, symnum);

      /* Some compilers put the variable decls inside of an
         LBRAC/RBRAC block.  This macro should be nonzero if this
	 is true.  DESC is N_DESC from the N_RBRAC symbol.
	 GCC_P is true if we've detected the GCC_COMPILED_SYMBOL
	 or the GCC2_COMPILED_SYMBOL.  */
#if !defined (VARIABLES_INSIDE_BLOCK)
#define VARIABLES_INSIDE_BLOCK(desc, gcc_p) 0
#endif

      /* Can only use new->locals as local symbols here if we're in
         gcc or on a machine that puts them before the lbrack.  */
      if (!VARIABLES_INSIDE_BLOCK(desc, processing_gcc_compilation))
	local_symbols = new->locals;

      if (context_stack_depth
	  > !VARIABLES_INSIDE_BLOCK(desc, processing_gcc_compilation))
	{
	  /* This is not the outermost LBRAC...RBRAC pair in the function,
	     its local symbols preceded it, and are the ones just recovered
	     from the context stack.  Define the block for them (but don't
	     bother if the block contains no symbols.  Should we complain
	     on blocks without symbols?  I can't think of any useful purpose
	     for them).  */
	  if (local_symbols != NULL)
	    {
	      /* Muzzle a compiler bug that makes end < start.  (which
		 compilers?  Is this ever harmful?).  */
	      if (new->start_addr > valu)
		{
		  complain (&lbrac_rbrac_complaint);
		  new->start_addr = valu;
		}
	      /* Make a block for the local symbols within.  */
	      finish_block (0, &local_symbols, new->old_blocks,
			    new->start_addr, valu, objfile);
	    }
	}
      else
	{
	  /* This is the outermost LBRAC...RBRAC pair.  There is no
	     need to do anything; leave the symbols that preceded it
	     to be attached to the function's own block.  We need to
	     indicate that we just moved outside of the function.  */
	  within_function = 0;
	}

      if (VARIABLES_INSIDE_BLOCK(desc, processing_gcc_compilation))
	/* Now pop locals of block just finished.  */
	local_symbols = new->locals;
      break;

    case N_FN:
    case N_FN_SEQ:
      /* This kind of symbol indicates the start of an object file.  */
      /* Relocate for dynamic loading */
      valu += ANOFFSET (section_offsets, SECT_OFF_TEXT);
      break;

    case N_SO:
      /* This type of symbol indicates the start of data
	 for one source file.
	 Finish the symbol table of the previous source file
	 (if any) and start accumulating a new symbol table.  */
      /* Relocate for dynamic loading */
      valu += ANOFFSET (section_offsets, SECT_OFF_TEXT);

      n_opt_found = 0;

#ifdef SUN_FIXED_LBRAC_BUG
      last_pc_address = valu;	/* Save for SunOS bug circumcision */
#endif

#ifdef PCC_SOL_BROKEN
      /* pcc bug, occasionally puts out SO for SOL.  */
      if (context_stack_depth > 0)
	{
	  start_subfile (name, NULL);
	  break;
	}
#endif
      if (last_source_file)
	{
	  /* Check if previous symbol was also an N_SO (with some
	     sanity checks).  If so, that one was actually the directory
	     name, and the current one is the real file name.
	     Patch things up. */	   
	  if (previous_stab_code == (unsigned char) N_SO)
	    {
	      patch_subfile_names (current_subfile, name);
	      break;		/* Ignore repeated SOs */
	    }
	  end_symtab (valu, 0, 0, objfile, SECT_OFF_TEXT);
	  end_stabs ();
	}
      start_stabs ();
      start_symtab (name, NULL, valu);
      break;


    case N_SOL:
      /* This type of symbol indicates the start of data for
	 a sub-source-file, one whose contents were copied or
	 included in the compilation of the main source file
	 (whose name was given in the N_SO symbol.)  */
      /* Relocate for dynamic loading */
      valu += ANOFFSET (section_offsets, SECT_OFF_TEXT);
      start_subfile (name, current_subfile->dirname);
      break;

    case N_BINCL:
      push_subfile ();
      add_new_header_file (name, valu);
      start_subfile (name, current_subfile->dirname);
      break;

    case N_EINCL:
      start_subfile (pop_subfile (), current_subfile->dirname);
      break;

    case N_EXCL:
      add_old_header_file (name, valu);
      break;

    case N_SLINE:
      /* This type of "symbol" really just records
	 one line-number -- core-address correspondence.
	 Enter it in the line list for this symbol table.  */
      /* Relocate for dynamic loading and for ELF acc fn-relative syms.  */
      valu += function_start_offset;
#ifdef SUN_FIXED_LBRAC_BUG
      last_pc_address = valu;	/* Save for SunOS bug circumcision */
#endif
      record_line (current_subfile, desc, valu);
      break;

    case N_BCOMM:
      common_block_start (name, objfile);
      break;

    case N_ECOMM:
      common_block_end (objfile);
      break;

    /* The following symbol types need to have the appropriate offset added
       to their value; then we process symbol definitions in the name.  */

    case N_STSYM:		/* Static symbol in data seg */
    case N_LCSYM:		/* Static symbol in BSS seg */
    case N_ROSYM:		/* Static symbol in Read-only data seg */
     /* HORRID HACK DEPT.  However, it's Sun's furgin' fault.
	Solaris2's stabs-in-elf makes *most* symbols relative
	but leaves a few absolute (at least for Solaris 2.1 and version
	2.0.1 of the SunPRO compiler).  N_STSYM and friends sit on the fence.
	.stab "foo:S...",N_STSYM 	is absolute (ld relocates it)
	.stab "foo:V...",N_STSYM	is relative (section base subtracted).
	This leaves us no choice but to search for the 'S' or 'V'...
	(or pass the whole section_offsets stuff down ONE MORE function
	call level, which we really don't want to do).  */
      {
	char *p;
	p = strchr (name, ':');
	if (p != 0 && p[1] == 'S')
	  {
	    /* The linker relocated it.  We don't want to add an
	       elfstab_offset_sections-type offset, but we *do* want
	       to add whatever solib.c passed to symbol_file_add as
	       addr (this is known to affect SunOS4, and I suspect ELF
	       too).  Since elfstab_offset_sections currently does not
	       muck with the text offset (there is no Ttext.text
	       symbol), we can get addr from the text offset.  If
	       elfstab_offset_sections ever starts dealing with the
	       text offset, and we still need to do this, we need to
	       invent a SECT_OFF_ADDR_KLUDGE or something.  */
	    valu += ANOFFSET (section_offsets, SECT_OFF_TEXT);
	    goto define_a_symbol;
	  }
	/* Since it's not the kludge case, re-dispatch to the right handler. */
	switch (type) {
	case N_STSYM: 	goto case_N_STSYM;
	case N_LCSYM:	goto case_N_LCSYM;
	case N_ROSYM:	goto case_N_ROSYM;
	default:	abort();
	}
      }

    case_N_STSYM:		/* Static symbol in data seg */
    case N_DSLINE:		/* Source line number, data seg */
      valu += ANOFFSET (section_offsets, SECT_OFF_DATA);
      goto define_a_symbol;

    case_N_LCSYM:		/* Static symbol in BSS seg */
    case N_BSLINE:		/* Source line number, bss seg */
    /*   N_BROWS:	overlaps with N_BSLINE */
      valu += ANOFFSET (section_offsets, SECT_OFF_BSS);
      goto define_a_symbol;

    case_N_ROSYM:		/* Static symbol in Read-only data seg */
      valu += ANOFFSET (section_offsets, SECT_OFF_RODATA);
      goto define_a_symbol;

    case N_ENTRY:		/* Alternate entry point */
      /* Relocate for dynamic loading */
      valu += ANOFFSET (section_offsets, SECT_OFF_TEXT);
      goto define_a_symbol;

    /* The following symbol types we don't know how to process.  Handle
       them in a "default" way, but complain to people who care.  */
    default:
    case N_CATCH:		/* Exception handler catcher */
    case N_EHDECL:		/* Exception handler name */
    case N_PC:			/* Global symbol in Pascal */
    case N_M2C:			/* Modula-2 compilation unit */
    /*   N_MOD2:	overlaps with N_EHDECL */
    case N_SCOPE:		/* Modula-2 scope information */
    case N_ECOML:		/* End common (local name) */
    case N_NBTEXT:		/* Gould Non-Base-Register symbols??? */
    case N_NBDATA:
    case N_NBBSS:
    case N_NBSTS:
    case N_NBLCS:
      complain (&unknown_symtype_complaint, local_hex_string (type));
      /* FALLTHROUGH */

    /* The following symbol types don't need the address field relocated,
       since it is either unused, or is absolute.  */
    define_a_symbol:
    case N_GSYM:		/* Global variable */
    case N_NSYMS:		/* Number of symbols (ultrix) */
    case N_NOMAP:		/* No map?  (ultrix) */
    case N_RSYM:		/* Register variable */
    case N_DEFD:		/* Modula-2 GNU module dependency */
    case N_SSYM:		/* Struct or union element */
    case N_LSYM:		/* Local symbol in stack */
    case N_PSYM:		/* Parameter variable */
    case N_LENG:		/* Length of preceding symbol type */
      if (name)
	{
	  int deftype;
	  char *colon_pos = strchr (name, ':');
	  if (colon_pos == NULL)
	    deftype = '\0';
	  else
	    deftype = colon_pos[1];

	  switch (deftype)
	    {
	    case 'f':
	    case 'F':
	      function_stab_type = type;

#ifdef SUN_FIXED_LBRAC_BUG
	      /* The Sun acc compiler, under SunOS4, puts out
		 functions with N_GSYM or N_STSYM.  The problem is
		 that the address of the symbol is no good (for N_GSYM
		 it doesn't even attept an address; for N_STSYM it
		 puts out an address but then it gets relocated
		 relative to the data segment, not the text segment).
		 Currently we can't fix this up later as we do for
		 some types of symbol in scan_file_globals.
		 Fortunately we do have a way of finding the address -
		 we know that the value in last_pc_address is either
		 the one we want (if we're dealing with the first
		 function in an object file), or somewhere in the
		 previous function. This means that we can use the
		 minimal symbol table to get the address.  */

	      /* On solaris up to 2.2, the N_FUN stab gets relocated.
		 On Solaris 2.3, ld no longer relocates stabs (which
		 is good), and the N_FUN's value is now always zero.
		 The following code can't deal with this, because
		 last_pc_address depends on getting the address from a
		 N_SLINE or some such and in Solaris those are function
		 relative.  Best fix is probably to create a Ttext.text symbol
		 and handle this like Ddata.data and so on.  */

	      if (type == N_GSYM || type == N_STSYM)
		{
		  struct minimal_symbol *m;
		  int l = colon_pos - name;

		  m = lookup_minimal_symbol_by_pc (last_pc_address);
		  if (m && STREQN (SYMBOL_NAME (m), name, l))
		    /* last_pc_address was in this function */
		    valu = SYMBOL_VALUE (m);
		  else if (m && STREQN (SYMBOL_NAME (m+1), name, l))
		    /* last_pc_address was in last function */
		    valu = SYMBOL_VALUE (m+1);
		  else
		    /* Not found - use last_pc_address (for finish_block) */
		    valu = last_pc_address;
		}

	      last_pc_address = valu;	/* Save for SunOS bug circumcision */
#endif

	      if (block_address_function_relative)
		/* For Solaris 2.0 compilers, the block addresses and
		   N_SLINE's are relative to the start of the
		   function.  On normal systems, and when using gcc on
		   Solaris 2.0, these addresses are just absolute, or
		   relative to the N_SO, depending on
		   BLOCK_ADDRESS_ABSOLUTE.  */
		function_start_offset = valu;	

	      within_function = 1;
	      if (context_stack_depth > 0)
		{
		  new = pop_context ();
		  /* Make a block for the local symbols within.  */
		  finish_block (new->name, &local_symbols, new->old_blocks,
				new->start_addr, valu, objfile);
		}
	      /* Stack must be empty now.  */
	      if (context_stack_depth != 0)
		complain (&lbrac_unmatched_complaint, symnum);

	      new = push_context (0, valu);
	      new->name = define_symbol (valu, name, desc, type, objfile);
	      break;

	    default:
	      define_symbol (valu, name, desc, type, objfile);
	      break;
	    }
	}
      break;

    /* We use N_OPT to carry the gcc2_compiled flag.  Sun uses it
       for a bunch of other flags, too.  Someday we may parse their
       flags; for now we ignore theirs and hope they'll ignore ours.  */
    case N_OPT:			/* Solaris 2:  Compiler options */
      if (name)
	{
	  if (STREQ (name, GCC2_COMPILED_FLAG_SYMBOL))
	    {
	      processing_gcc_compilation = 2;
#if 1	      /* Works, but is experimental.  -fnf */
	      if (AUTO_DEMANGLING)
		{
		  set_demangling_style (GNU_DEMANGLING_STYLE_STRING);
		}
#endif
	    }
	  else
	    n_opt_found = 1;
	}
      break;

    /* The following symbol types can be ignored.  */
    case N_OBJ:			/* Solaris 2:  Object file dir and name */
    /*   N_UNDF: 		   Solaris 2:  file separator mark */
    /*   N_UNDF: -- we will never encounter it, since we only process one
		    file's symbols at once.  */
    case N_ENDM:		/* Solaris 2:  End of module */
    case N_MAIN:		/* Name of main routine.  */
      break;
    }

  previous_stab_code = type;
}

/* FIXME: The only difference between this and elfstab_build_psymtabs is
   the call to install_minimal_symbols for elf.  If the differences are
   really that small, the code should be shared.  */

/* Scan and build partial symbols for an coff symbol file.
   The coff file has already been processed to get its minimal symbols.

   This routine is the equivalent of dbx_symfile_init and dbx_symfile_read
   rolled into one.

   OBJFILE is the object file we are reading symbols from.
   ADDR is the address relative to which the symbols are (e.g.
   the base address of the text segment).
   MAINLINE is true if we are reading the main symbol
   table (as opposed to a shared lib or dynamically loaded file).
   STABOFFSET and STABSIZE define the location in OBJFILE where the .stab
   section exists.
   STABSTROFFSET and STABSTRSIZE define the location in OBJFILE where the
   .stabstr section exists.

   This routine is mostly copied from dbx_symfile_init and dbx_symfile_read,
   adjusted for coff details. */

void
coffstab_build_psymtabs (objfile, section_offsets, mainline, 
			       staboffset, stabsize,
			       stabstroffset, stabstrsize)
      struct objfile *objfile;
      struct section_offsets *section_offsets;
      int mainline;
      file_ptr staboffset;
      unsigned int stabsize;
      file_ptr stabstroffset;
      unsigned int stabstrsize;
{
  int val;
  bfd *sym_bfd = objfile->obfd;
  char *name = bfd_get_filename (sym_bfd);
  struct dbx_symfile_info *info;

  /* There is already a dbx_symfile_info allocated by our caller.
     It might even contain some info from the coff symtab to help us.  */
  info = (struct dbx_symfile_info *) objfile->sym_stab_info;

  DBX_TEXT_SECT (objfile) = bfd_get_section_by_name (sym_bfd, ".text");
  if (!DBX_TEXT_SECT (objfile))
    error ("Can't find .text section in symbol file");

#define	COFF_STABS_SYMBOL_SIZE	12	/* XXX FIXME XXX */
  DBX_SYMBOL_SIZE    (objfile) = COFF_STABS_SYMBOL_SIZE;
  DBX_SYMCOUNT       (objfile) = stabsize / DBX_SYMBOL_SIZE (objfile);
  DBX_STRINGTAB_SIZE (objfile) = stabstrsize;
  DBX_SYMTAB_OFFSET  (objfile) = staboffset;
  
  if (stabstrsize > bfd_get_size (sym_bfd))
    error ("ridiculous string table size: %d bytes", stabstrsize);
  DBX_STRINGTAB (objfile) = (char *)
    obstack_alloc (&objfile->psymbol_obstack, stabstrsize+1);

  /* Now read in the string table in one big gulp.  */

  val = bfd_seek (sym_bfd, stabstroffset, SEEK_SET);
  if (val < 0)
    perror_with_name (name);
  val = bfd_read (DBX_STRINGTAB (objfile), stabstrsize, 1, sym_bfd);
  if (val != stabstrsize)
    perror_with_name (name);

  stabsread_new_init ();
  buildsym_new_init ();
  free_header_files ();
  init_header_files ();

  processing_acc_compilation = 1;

  /* In a coff file, we've already installed the minimal symbols that came
     from the coff (non-stab) symbol table, so always act like an
     incremental load here. */
  dbx_symfile_read (objfile, section_offsets, 0);
}

/* Scan and build partial symbols for an ELF symbol file.
   This ELF file has already been processed to get its minimal symbols,
   and any DWARF symbols that were in it.

   This routine is the equivalent of dbx_symfile_init and dbx_symfile_read
   rolled into one.

   OBJFILE is the object file we are reading symbols from.
   ADDR is the address relative to which the symbols are (e.g.
   the base address of the text segment).
   MAINLINE is true if we are reading the main symbol
   table (as opposed to a shared lib or dynamically loaded file).
   STABOFFSET and STABSIZE define the location in OBJFILE where the .stab
   section exists.
   STABSTROFFSET and STABSTRSIZE define the location in OBJFILE where the
   .stabstr section exists.

   This routine is mostly copied from dbx_symfile_init and dbx_symfile_read,
   adjusted for elf details. */

void
elfstab_build_psymtabs (objfile, section_offsets, mainline, 
			       staboffset, stabsize,
			       stabstroffset, stabstrsize)
      struct objfile *objfile;
      struct section_offsets *section_offsets;
      int mainline;
      file_ptr staboffset;
      unsigned int stabsize;
      file_ptr stabstroffset;
      unsigned int stabstrsize;
{
  int val;
  bfd *sym_bfd = objfile->obfd;
  char *name = bfd_get_filename (sym_bfd);
  struct dbx_symfile_info *info;

  /* There is already a dbx_symfile_info allocated by our caller.
     It might even contain some info from the ELF symtab to help us.  */
  info = (struct dbx_symfile_info *) objfile->sym_stab_info;

  DBX_TEXT_SECT (objfile) = bfd_get_section_by_name (sym_bfd, ".text");
  if (!DBX_TEXT_SECT (objfile))
    error ("Can't find .text section in symbol file");

#define	ELF_STABS_SYMBOL_SIZE	12	/* XXX FIXME XXX */
  DBX_SYMBOL_SIZE    (objfile) = ELF_STABS_SYMBOL_SIZE;
  DBX_SYMCOUNT       (objfile) = stabsize / DBX_SYMBOL_SIZE (objfile);
  DBX_STRINGTAB_SIZE (objfile) = stabstrsize;
  DBX_SYMTAB_OFFSET  (objfile) = staboffset;
  
  if (stabstrsize > bfd_get_size (sym_bfd))
    error ("ridiculous string table size: %d bytes", stabstrsize);
  DBX_STRINGTAB (objfile) = (char *)
    obstack_alloc (&objfile->psymbol_obstack, stabstrsize+1);

  /* Now read in the string table in one big gulp.  */

  val = bfd_seek (sym_bfd, stabstroffset, SEEK_SET);
  if (val < 0)
    perror_with_name (name);
  val = bfd_read (DBX_STRINGTAB (objfile), stabstrsize, 1, sym_bfd);
  if (val != stabstrsize)
    perror_with_name (name);

  stabsread_new_init ();
  buildsym_new_init ();
  free_header_files ();
  init_header_files ();
  install_minimal_symbols (objfile);

  processing_acc_compilation = 1;

  /* In an elf file, we've already installed the minimal symbols that came
     from the elf (non-stab) symbol table, so always act like an
     incremental load here. */
  dbx_symfile_read (objfile, section_offsets, 0);
}

/* Scan and build partial symbols for a PA symbol file.
   This PA file has already been processed to get its minimal symbols.

   OBJFILE is the object file we are reading symbols from.
   ADDR is the address relative to which the symbols are (e.g.
   the base address of the text segment).
   MAINLINE is true if we are reading the main symbol
   table (as opposed to a shared lib or dynamically loaded file).

   */

void
pastab_build_psymtabs (objfile, section_offsets, mainline)
     struct objfile *objfile;
     struct section_offsets *section_offsets;
     int mainline;
{
  free_header_files ();
  init_header_files ();

  /* This is needed to debug objects assembled with gas2.  */
  processing_acc_compilation = 1;

  /* In a PA file, we've already installed the minimal symbols that came
     from the PA (non-stab) symbol table, so always act like an
     incremental load here. */

  dbx_symfile_read (objfile, section_offsets, mainline);
}

/* Parse the user's idea of an offset for dynamic linking, into our idea
   of how to represent it for fast symbol reading.  */

static struct section_offsets *
dbx_symfile_offsets (objfile, addr)
     struct objfile *objfile;
     CORE_ADDR addr;
{
  struct section_offsets *section_offsets;
  int i;

  objfile->num_sections = SECT_OFF_MAX;
  section_offsets = (struct section_offsets *)
    obstack_alloc (&objfile -> psymbol_obstack,
		   sizeof (struct section_offsets)
		   + sizeof (section_offsets->offsets) * (SECT_OFF_MAX-1));

  for (i = 0; i < SECT_OFF_MAX; i++)
    ANOFFSET (section_offsets, i) = addr;
  
  return section_offsets;
}

static struct sym_fns aout_sym_fns =
{
  bfd_target_aout_flavour,
  dbx_new_init,		/* sym_new_init: init anything gbl to entire symtab */
  dbx_symfile_init,	/* sym_init: read initial info, setup for sym_read() */
  dbx_symfile_read,	/* sym_read: read a symbol file into symtab */
  dbx_symfile_finish,	/* sym_finish: finished with file, cleanup */
  dbx_symfile_offsets,	/* sym_offsets: parse user's offsets to internal form */
  NULL			/* next: pointer to next struct sym_fns */
};

void
_initialize_dbxread ()
{
  add_symtab_fns(&aout_sym_fns);
}
