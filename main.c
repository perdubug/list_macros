/*
 * A utility to scan source project to list all macros and where to define&use them
 *
 * Example output:
 * --------------------------------------------------------------------------------------------------
 * Macro      : ENABLE_APPLE_STORE
 * Defined in : $ISA_SW_TOP/product_config/s40_10_4_cs16_family/config/tp_product.h :   1
 *              $ISA_SW_TOP/product_config/s40_10_4_family/config/tp_product.h :    2
 *              $ISA_SW_TOP/product_config/s40_11_2_cs16_family/config/tp_product.h :  3
 * Found from : $ISA_SW_TOP/common_sw/adaptation_isa_sw/audio_sw_s60/acc_s60/acc_int_conf.h
 *              $ISA_SW_TOP/common_sw/adaptation_isa_sw/baseband_components/em_sos/em_srv.h
 *              $ISA_SW_TOP/common_sw/adaptation_isa_sw/baseband_components/et/em/em_srv.h
 *              $ISA_SW_TOP/common_sw/adaptation_isa_sw/baseband_components/memory/nvd/nvd_srv.c
 *
 *
 * TODO:
 *       - scan just one time to get both define-in and found-from info...
 *       - support this format: #if define(xxx) and #if !define(xxx).Currently only support #ifdef and #ifndef
 *       - move skip_files stuff into an individual file for easy adding by user
 *       - support * in skip_files,like 'test_*' - skip all files begin with 'test_'
 */

/*
 *
 * 1  EXTERNAL RESOURCES
 *
 *     1.1 Include Files
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>

/*  1   GLOBAL */
unsigned long g_file_nums;  /* how many files be processed. for outputting summary information only */
unsigned long g_macro_nums; /* how many macros we found. for outputting summary information only */


/*  2   LOCAL CONSTANTS AND MACROS  */
//#define DEBUG
#define MAX_PATH_LEN 512
#define MAX_LINE_LEN 512*2

#define MAX_MACRO_NAME_LEN  512
#define MAX_MACRO_VALUE_LEN 512
#define MAX_CHARACTER_NUMS ('Z'-'A'+1+1)

/* put all illegal characters contains in the macro name here...MUST END BY '\0' */
const char _illegal_chars[] = {'(',')','\\','"','#','*','{','}','\0'};

const char * _skip_files[] = { "testscript_dodo.c",
                               NULL };

/*  3   MODULE DATA STRUCTURES      */
typedef struct FILE_INFO_NODE {

   char                    path[MAX_PATH_LEN];
   struct FILE_INFO_NODE * next;

}FILE_INFO_NODE;

typedef struct DEFINE_INFO_NODE {

   char       * fpath;     /* the path of the file that defined the macro */
   char       * value;     /* value of the macro. e.g. #define MAX_PATH 256, the value is 256 */
   unsigned int ln;        /* line number */

   struct DEFINE_INFO_NODE * next;

}DEFINE_INFO_NODE;

typedef struct FOUND_INFO_NODE {

   char       * fpath;     /* the path of the file that used the macro */
   unsigned int ln;        /* line number */

   struct FOUND_INFO_NODE * next;

}FOUND_INFO_NODE;

typedef struct MACRO_INFO_NODE {

   char               name[MAX_MACRO_NAME_LEN];  /* macro name */
   DEFINE_INFO_NODE * di;        /* the macro defined in where and its value,line number */
   FOUND_INFO_NODE  * fi;        /* the macro found from where and its line number */

   struct MACRO_INFO_NODE * next;

}MACRO_INFO_NODE,* PMACRO_INFO_NODE;

typedef struct MACRO_MATRIX_ELEMENT {

  MACRO_INFO_NODE * header;  /* point to the first node of macro infor linker(see MACRO_INFO_NODE) */
  // MACRO_INFO_NODE * last;    /* point to the last node of macro infor linker(see MACRO_INFO_NODE) */

}MACRO_MATRIX_ELEMENT;

/*  4   Local Function Prototypes  */
static char append_define_info_into_linker(MACRO_INFO_NODE * pheader,char * macro_name,char * hostfile,char * value,unsigned int line_number);
static void append_define_info_into_matrix(char * single_file, MACRO_MATRIX_ELEMENT * macro_matrix);
static char append_found_from_info_into_linker(MACRO_INFO_NODE * pheader,char * macro_name,char * hostfile,unsigned int line_number);
static void append_found_from_info_into_matrix(char * single_file, MACRO_MATRIX_ELEMENT * macro_matrix);
static void dump_macro_matrix(MACRO_MATRIX_ELEMENT * macro_matrix);
static void free_macro_matrix(MACRO_MATRIX_ELEMENT * macro_matrix);
static unsigned int macro_have_illegal_characters(char * str);
static char * rtrim(char * str);
static char * ltrim(char * str);

/*  5   MODULE CODE */

int main(int argc, char * argv[])
{
   FILE * fd = NULL;
   char fpath[MAX_PATH_LEN] = {0};

   FILE_INFO_NODE * pfin_header = NULL;
   FILE_INFO_NODE * pfin_cursor = NULL;

   unsigned int idx;
   unsigned int bcontinue;

   /* A to Z plus '_', each element points to a linker header that contain all macros infor(name,defined in,found from) with same capital letter
      macro_matrix[0] is all macros begin 'A'
      macro_matrix[25] is all macros begin 'Z'
      macro_matrix[26] is all macros begin '_'
    */
   MACRO_MATRIX_ELEMENT  macro_matrix[MAX_CHARACTER_NUMS];

   for(idx = 0; idx < MAX_CHARACTER_NUMS; idx++) {
      macro_matrix[idx].header = NULL;
   }

   /* Step 1. Find all files(*.c,*.cc,*.cpp,*.h,*.hi,*.inc) that may contain macro with full path */
   /*------------------------------------------------------------------------------------------------*/
   const char cmd_get_all_files[] = "find $PWD \\( -iname \"*.c\" -o -iname \"*.cc\" -o -iname \"*.cpp\" -o -iname \"*.h\" -o -iname \"*.hi\" -o -iname \"*.inc\" \\) > src_files";
   /* const char cmd_remove_newline_character[] = "perl -p -e 's/\\s+$/ /g' src_files"; */


#ifdef DEBUG
  fprintf(stdout,"Generating input file list...\n%s\n",cmd_get_all_files);
#endif

   if (system(cmd_get_all_files) != 0) {
      fprintf(stderr, "Can not get reference file list, please check your shell or path\n");
      exit(0);
   }

/*
   Remove newline character from generated file....

   if (system(cmd_remove_newline_character) != 0) {
      fprintf(stderr, "Remove newline characters failed\n");
      exit(0);
   }
*/

   /* Step 2. Open the file generated at the first step
    *        build macro matrix by going though each file list in the generated file. NOTE: Each line is a file path
    *        the macro matrix is a 2-D pointer array that contains macro infor(name,defined in,found from...) according to a-z order including '_',like

      'A' ABBA_ENABLE_VIDEO--->AND_X_SUPPORT--->NULL
           |
      'B' NULL
           |
      'C' NULL
           |
      'D' DOOM_ENABLE--->NULL
           |
      'E' EFFECT_C_SUPPORT--->EPROM_SUPPORT--->NULL
           |
      'F' NULL
           |
          ...
           |
      'W' WTF_SUPPORT--->NULL
           |
          ...
      'Z' NULL
           |
      '_' _NTF_SUPPORT-->NULL
    *
    */
   /*------------------------------------------------------------------------------------------------*/
   if ((fd = fopen("src_files","r")) == 0) {
        fprintf(stderr,"Read src_files failed:%s\n",strerror(errno));
        exit(0);
   }

   /* go through the file(src_files) line by line and put each line(a file's fullpath) into a linker */
   g_file_nums = 0;
   while (fgets(fpath, MAX_PATH_LEN, fd) != 0) {

       idx = 0;
       bcontinue = 0;

       /* make sure the file is not in skip list */
       while (_skip_files[idx] != NULL) {

           if (strstr(fpath,_skip_files[idx++]) != NULL) {
               bcontinue = 1;
               break;
           }
       }

       if (bcontinue == 1)  {
           continue;
       }

       if (pfin_header == NULL)  {
             pfin_header = malloc(sizeof(FILE_INFO_NODE));
             if (pfin_header == NULL) {
                 assert(1);
             }

             strncpy(pfin_header->path,fpath,MAX_PATH_LEN);
             pfin_header->next  = NULL;

             pfin_cursor = pfin_header;
       } else {
             FILE_INFO_NODE * p = malloc(sizeof(FILE_INFO_NODE));
             if (p == NULL) {
                 assert(1);
             }

             strncpy(p->path,fpath,MAX_PATH_LEN);
             p->next = NULL;

             pfin_cursor->next = p;
             pfin_cursor       = pfin_cursor->next;
       }

       g_file_nums++;
   }

   fclose(fd);

   g_macro_nums = 0;

   /* get 'found from' infor by going through the linker to process each node(a file's fullpath) */
   pfin_cursor = pfin_header;
   while (pfin_cursor != NULL) {
      append_found_from_info_into_matrix(pfin_cursor->path,&macro_matrix[0]);
      pfin_cursor = pfin_cursor->next;
   }

   /* get 'define in' infot by going through the linker to process each node(a file's fullpath) */
   pfin_cursor = pfin_header;
   while (pfin_cursor != NULL) {
      append_define_info_into_matrix(pfin_cursor->path,&macro_matrix[0]);
      pfin_cursor = pfin_cursor->next;
   }

   /* Step 3.Dump macro matrix into local file */
   /*------------------------------------------------------------------------------------------------*/
   dump_macro_matrix(&macro_matrix[0]);

   /* Step 4.Release all resources */
   /*------------------------------------------------------------------------------------------------*/
   /* free the memory for saving file path */
   while (pfin_header != NULL) {
      pfin_cursor = pfin_header;
      pfin_header = pfin_header->next;
      free(pfin_cursor);
   }

   free_macro_matrix(&macro_matrix[0]);

   return 1;
}

static void free_macro_matrix(MACRO_MATRIX_ELEMENT * macro_matrix)
{
    unsigned int i;
    MACRO_INFO_NODE * pcursor;

    MACRO_INFO_NODE * free_cursor = NULL;
    DEFINE_INFO_NODE * free_di;
    FOUND_INFO_NODE  * free_fi;

    if (macro_matrix == NULL) {
        return;
    }

    /*
        go through macro matrix {
            go through macro info linker  {
                go through di(defined info) linker
                go through fi(found from info) linker
            }
        }
     */

    /* start going through macro matrix...from A to Z and special characters,like '_' */
    for(i = 0; i < MAX_CHARACTER_NUMS; i++) {

        if (macro_matrix[i].header == NULL) {
            continue;
        }

        pcursor = macro_matrix[i].header;

        /* start going through macro info linker start... */
        while (pcursor != NULL) {

            free_cursor = pcursor;

            /* free di and its value if has */
            while (pcursor->di != NULL) {

                free_di = pcursor->di;
                pcursor->di = pcursor->di->next;

                if (free_di->value != NULL) {
                    free(free_di->value);
                }
                free(free_di);
            }


            /* free fi */
            while (pcursor->fi != NULL) {

                free_fi = pcursor->fi;
                pcursor->fi = pcursor->fi->next;

                free(free_fi);
            }

            /* next one... */
            pcursor = pcursor->next;

            free(free_cursor);
        }
    } /* end for */
}

/*
    returns 0 means no illegal characters in macro name
    otherwise returns 1
 */
static unsigned int macro_have_illegal_characters(char * str)
{
    unsigned int ret = 0;
    unsigned int i   = 0;

    char * ptr = str;

    assert(str != NULL);

    /* space also means the macro name string is end */
    while (*ptr != ' ' && *ptr != '\0' ) {
        i = 0;

        while( _illegal_chars[i] != '\0') {
            if (*ptr == _illegal_chars[i++]) {
                ret = 1;
                goto GOOOO;
            }
        }

       ptr++;
    }

GOOOO:
    return ret;
}


static void dump_macro_matrix(MACRO_MATRIX_ELEMENT * macro_matrix)
{
    unsigned int i;
    MACRO_INFO_NODE * pcursor;

    assert(macro_matrix != NULL);

    /*
        go through macro matrix {
            go through macro info linker  {
                go through di(defined info) linker
                go through fi(found from info) linker
            }
        }
     */

    /* start going through macro matrix...from A to Z and special characters,like '_' */
    for(i = 0; i < MAX_CHARACTER_NUMS; i++) {

        if (macro_matrix[i].header == NULL) {
            continue;
        }

        pcursor = macro_matrix[i].header;

        /* start going through macro info linker start... */
        while (pcursor != NULL) {

            /* Step 1. output macro name */
            fprintf(stdout, "Macro:  %s\n",pcursor->name);

            /* Step 2. output defined-in infor */
            fprintf(stdout, "Defined in:\n");
            while (pcursor->di != NULL) {

                 fprintf(stdout,"Line%d:%s    %s\n",
                         pcursor->di->ln,
                         pcursor->di->fpath,
                         (pcursor->di->value != NULL) ? pcursor->di->value : " ");

                 pcursor->di = pcursor->di->next;
            }
            fprintf(stdout, "\n");

            /* Step 3. output found-from infor */
            fprintf(stdout, "Found from:\n");
            while (pcursor->fi != NULL) {

                 fprintf(stdout,"Line%d:%s\n",
                         pcursor->fi->ln,
                         pcursor->fi->fpath);

                 pcursor->fi = pcursor->fi->next;
            }
            fprintf(stdout, "-------------------------------------------\n");

            /* next one... */
            pcursor = pcursor->next;
        }
    } /* end for */

    /* output summary information */
    fprintf(stdout,"\n------------------------------------------\n");
    fprintf(stdout,"processed files:%lu\nprocessed macro:%lu\n",g_file_nums,g_macro_nums);
}

static void append_found_from_info_into_matrix(char * single_file,                      /* in     */
                                               MACRO_MATRIX_ELEMENT  * macro_matrix)    /* in/out */
{
    FILE * fd = NULL;
    char line[MAX_LINE_LEN] = {0};
    char * pcursor;  /* the cursor for the current line we are processing */
    unsigned int line_number = 0;
    unsigned int bcontinue = 0;
    char macro_mname[MAX_MACRO_NAME_LEN];
    int idx = 0;

    char * pure_path = rtrim(single_file);

    if ((fd = fopen(pure_path,"r")) == 0) {
        fprintf(stderr,"Read file(%s) failed:%s\n",pure_path,strerror(errno));
        exit(0);
    }

#ifdef DEBUG
    fprintf(stdout,"Scanning %s.....\n",pure_path);
    fprintf(stdout,"-----------------------------\n");
#endif

    /* go through file line by line for searching '#ifdef' and '#ifndef' no matter how many spaces between '#' and 'if'
       e.g. #ifdef
            # ifdef
            #   ifdef
     */
    while (fgets(line, MAX_LINE_LEN, fd) != 0) {

       bcontinue = 0;

       line_number++;

       pcursor = ltrim(line);

       if (*pcursor++ != '#') {
           /* illegal: '#' is not the first available character */
           continue;
       }

       if (*pcursor != 'i' &&
           !isspace((int)*pcursor)) {
           /* illegal: only space or character 'i' can follow '#' */
           continue;
       }

       /* ignore spaces behind '#' */
       while (isspace((int)*pcursor)) {
           pcursor++;
       }

       if ((pcursor = strstr(line,"ifdef")) == NULL &&
           (pcursor = strstr(line,"ifndef")) == NULL ) {
           /* illegal: no 'ifdef' and 'ifndef' follow '#' */
           continue;
       }

       if ((pcursor = strchr(pcursor,' ')) == NULL) {
           /* illegal: no space(s) follow #ifdef or #ifndef */
           continue;
       }

       /* ignore spaces between ifdef/ifndef and macro name */
       while (isspace((int)*pcursor)) {
           pcursor++;
       }

       /* get macro name...

          continue if the macro is for header file protection only,like
          #ifnde  FOO_H
          #define FOO_H
          or
          #ifnde  FOO_H_
          #define FOO_H_
        */
       memset(macro_mname,0x0,MAX_MACRO_NAME_LEN);

       idx = 0;
       while( pcursor != NULL && *pcursor != ' ' && *pcursor != '\0') {

           if (*pcursor == '/' &&
               (*(pcursor+1) != ' ' && *(pcursor+1) != '\0' && (*(pcursor+1) == '*' || *(pcursor+1) == '/')) )
           {
               /* stop seeking if meets 'slash*' and '//' in macro value */
               break;
           }

           if (*pcursor == '_' &&
               (*(pcursor+1) != ' ' && *(pcursor+1) != '\0' && (*(pcursor+1) == 'H' || *(pcursor+1) == 'h')) &&
               (*(pcursor+2) == ' ' || *(pcursor+2) == 0x0A || *(pcursor+2) == 0x0D || *(pcursor+2) == '_') )
           {
               bcontinue = 1;
               break;
           }

           macro_mname[idx++] = *pcursor++;
       }

       if (bcontinue == 1) {
           continue;
       }

       /* make sure no illegal characters in macro name */
       //if (macro_have_illegal_characters(macro_mname)) {
       //    continue;
       //}

       /* get a valid macro name that follows ifdef or ifndef */
#ifdef DEBUG
       fprintf(stdout,"%s at line%d\n",rtrim(macro_mname),line_number);
#endif

       if (macro_mname[0] - 'a' >= 0 && macro_mname[0] - 'z' <= 0) {
           idx = macro_mname[0] - 'a';
       } else if (macro_mname[0] - 'A' >= 0 && macro_mname[0] - 'Z' <= 0) {
           idx = macro_mname[0] - 'A';
       } else if (macro_mname[0] == '_') {
           idx = 26;
       } else {
           assert(1);
       }

       if (macro_matrix[idx].header == NULL)
       {
           macro_matrix[idx].header = (MACRO_INFO_NODE *)malloc(sizeof(MACRO_INFO_NODE));
           if (macro_matrix[idx].header == NULL) {
               assert(1);
           }

           strncpy(macro_matrix[idx].header->name,rtrim(macro_mname),MAX_MACRO_NAME_LEN);
           macro_matrix[idx].header->di = NULL;  /* we don't know di infor yet,will be done in next phase */

           macro_matrix[idx].header->fi = (FOUND_INFO_NODE *)malloc(sizeof(FOUND_INFO_NODE));
           if (macro_matrix[idx].header->fi == NULL) {
               assert(1);
           }

           macro_matrix[idx].header->fi->fpath = single_file;
           macro_matrix[idx].header->fi->ln    = line_number;
           macro_matrix[idx].header->next  = NULL;

       } else {
           append_found_from_info_into_linker(macro_matrix[idx].header,
                                              rtrim(macro_mname),single_file,line_number);
       }

       g_macro_nums++;

    } /* end while(fgets(...)) */

    fclose(fd);
}

/*
 * returns 0 means the node has been inserted into linker
 * returns 1 means the node is dulicated with the existing one in the linker
 */
static char append_found_from_info_into_linker(MACRO_INFO_NODE * pheader,   /* in, the target linker's header */
                                               char * macro_name,           /* in, macro name */
                                               char * hostfile,             /* in, macro in which file */
                                               unsigned int line_number)    /* in, the line number of the macro in the hostfile */
{
    MACRO_INFO_NODE * pcursor = pheader;
    MACRO_INFO_NODE * pinspos = pheader;

    MACRO_INFO_NODE * newnode_min = NULL;
    FOUND_INFO_NODE * newnode_fin = NULL;
    FOUND_INFO_NODE * fin_cursor  = NULL;
    FOUND_INFO_NODE * fin_inspos  = NULL;

    char ret = 0;

    assert(pheader    != NULL);
    assert(macro_name != NULL);
    assert(hostfile  != NULL);

    while (pcursor != NULL && strcmp(pcursor->name, macro_name) != 0) {
        pinspos = pcursor;
        pcursor = pcursor->next;
    }

    if (pcursor == NULL) {
        /* we don have the macro yet... */
        newnode_min = (MACRO_INFO_NODE *)malloc(sizeof(MACRO_INFO_NODE));
        if (newnode_min == NULL) {
            assert(1);
        }

        strncpy(newnode_min->name,macro_name,MAX_MACRO_NAME_LEN);

        newnode_min->fi = (FOUND_INFO_NODE *)malloc(sizeof(FOUND_INFO_NODE));
        if (newnode_min->fi == NULL) {
            assert(1);
        }

        newnode_min->fi->fpath = hostfile;
        newnode_min->fi->ln    = line_number;
        newnode_min->fi->next  = NULL;

        newnode_min->di   = NULL;
        newnode_min->next = NULL;

        pinspos->next = newnode_min;

    } else {

        if (pcursor->fi == NULL) {
            /* we get a matched item,but its fi is empty */
            newnode_fin = (FOUND_INFO_NODE *)malloc(sizeof(FOUND_INFO_NODE));
            if (newnode_fin == NULL) {
                assert(1);
            }

            newnode_fin->fpath = hostfile;
            newnode_fin->ln    = line_number;
            newnode_fin->next  = NULL;

            pcursor->fi = newnode_fin;
        } else {

            fin_cursor = pcursor->fi;
            /* we already have the macro in the linker. trying to insert found infor into the pcursor's fi linker */
            while (fin_cursor != NULL &&
                   (fin_cursor->fpath != hostfile || fin_cursor->ln != line_number) )
            {
                fin_inspos  = fin_cursor;
                fin_cursor  = fin_cursor->next;
            }

            if (fin_cursor == NULL) {
                /* we are in the end of linker... */
                newnode_fin = (FOUND_INFO_NODE *)malloc(sizeof(FOUND_INFO_NODE));
                if (newnode_fin == NULL) {
                    assert(1);
                }

                newnode_fin->fpath = hostfile;
                newnode_fin->ln    = line_number;
                newnode_fin->next  = NULL;

                fin_inspos->next = newnode_fin;
           } else {
                ret = 1;
           }
        }

   }

   return ret;
}

static void append_define_info_into_matrix(char * single_file, MACRO_MATRIX_ELEMENT * macro_matrix)
{
    FILE * fd = NULL;

    char * pcursor;  /* the cursor for the current line we are processing */

    unsigned int line_number = 0;
    unsigned int idx = 0;
    unsigned int bcontinue = 0;

    char * pure_path = rtrim(single_file);
    char * macro_value; /* macro value. e.g. for '#define VX_SUPPORT TRUE',its value is 'TRUE' */

    char macro_mname[MAX_MACRO_NAME_LEN];
    char line[MAX_LINE_LEN] = {0};

    if ((fd = fopen(pure_path,"r")) == 0) {
        fprintf(stderr,"Read file(%s) failed:%s\n",pure_path,strerror(errno));
        exit(0);
    }

#ifdef DEBUG
    fprintf(stdout,"Scanning %s.....\n",pure_path);
    fprintf(stdout,"-----------------------------\n");
#endif

    /* go through file line by line for searching '#define' no matter how many spaces between '#' and 'if'
       e.g. #define
            # define
     */
    while (fgets(line, MAX_LINE_LEN, fd) != 0) {

       bcontinue = 0;

       line_number++;

       pcursor = ltrim(line);

       if (*pcursor++ != '#') {
           /* illegal: '#' is not the first available character */
           continue;
       }

       if (*pcursor != 'd' &&
           !isspace((int)*pcursor)) {
           /* illegal: only space or character 'd' can follow '#' */
           continue;
       }

       /* ignore spaces behind '#' */
       while (isspace((int)*pcursor)) {
           pcursor++;
       }

       if ((pcursor = strstr(line,"define")) == NULL) {
           /* illegal: no 'define' follows '#' */
           continue;
       }

       if ((pcursor = strchr(pcursor,' ')) == NULL) {
           /* illegal: no space(s) follow '#idefine' */
           continue;
       }

       /* ignore spaces between define and macro name */
       while (isspace((int)*pcursor)) {
           pcursor++;
       }

       memset(macro_mname,0x0,MAX_MACRO_NAME_LEN);

       /* get macro name...

          continue if the macro is for header file protection only,like
          #ifnde  FOO_H
          #define FOO_H
          or
          #ifnde  FOO_H_
          #define FOO_H_
        */

       idx = 0;
       while( pcursor != NULL && *pcursor != ' ' && *pcursor != '\0') {

           if (*pcursor == '_' &&
               (*(pcursor+1) != ' ' && *(pcursor+1) != '\0' && (*(pcursor+1) == 'H' || *(pcursor+1) == 'h')) &&
               (*(pcursor+2) == ' ' || *(pcursor+2) == 0x0A || *(pcursor+2) == 0x0D || *(pcursor+2) == '_') )
           {
               bcontinue = 1;
               break;
           }

           macro_mname[idx++] = *pcursor++;
       }

       if (bcontinue == 1) {
           continue;
       }

       /* make sure no illegal characters in macro name */
       if (macro_have_illegal_characters(macro_mname)) {
           continue;
       }

       /* ignore spaces between macro name and its value */
       while (isspace((int)*pcursor)) {
           pcursor++;
       }

       /* get macro value name */
       idx = 0;
       macro_value = malloc(MAX_MACRO_VALUE_LEN);
       if (macro_value == NULL) {
           assert(1);
       }

       memset(macro_value,'\0',MAX_MACRO_VALUE_LEN);

       while( pcursor != NULL && *pcursor != ' ' && *pcursor != '\0') {

           if (*pcursor == '/' &&
               (*(pcursor+1) != ' ' && *(pcursor+1) != '\0' && (*(pcursor+1) == '*' || *(pcursor+1) == '/')) )
           {
               /* stop seeking if meets 'slash*' and '//' in macro value */
               break;
           }

           macro_value[idx++] = *pcursor++;
       }

       if (idx == 0) {
           free(macro_value);
           macro_value = NULL;
       } else if (idx > MAX_MACRO_VALUE_LEN) {
           /* memory overwrite,please consider increase MAX_MACRO_VALUE_LEN */
           assert(1);
       }

       /* make sure no illegal characters in macro value */
       if (macro_value != NULL && macro_have_illegal_characters(macro_value)) {
           free(macro_value);
           macro_value = NULL;
           continue;
       }

       /* get a valid macro name that follows define */
#ifdef DEBUG
       fprintf(stdout,">>%s,%s,%d\n",rtrim(macro_mname),macro_value == NULL?"":rtrim(macro_value),line_number);
#endif

       if (macro_mname[0] - 'a' >= 0 && macro_mname[0] - 'z' <= 0) {
           idx = macro_mname[0] - 'a';
       } else if (macro_mname[0] - 'A' >= 0 && macro_mname[0] - 'Z' <= 0) {
           idx = macro_mname[0] - 'A';
       } else if (macro_mname[0] == '_') {
           idx = 26;
       } else {
           assert(1);
       }

       if (macro_matrix[idx].header == NULL)
       {
           macro_matrix[idx].header = (MACRO_INFO_NODE *)malloc(sizeof(MACRO_INFO_NODE));
           if (macro_matrix[idx].header == NULL) {
               assert(1);
           }

           strncpy(macro_matrix[idx].header->name,rtrim(macro_mname),MAX_MACRO_NAME_LEN);
           macro_matrix[idx].header->fi = NULL;  /* not in this phase */

           macro_matrix[idx].header->di = (DEFINE_INFO_NODE *)malloc(sizeof(DEFINE_INFO_NODE));
           if (macro_matrix[idx].header->di == NULL) {
               assert(1);
           }

           macro_matrix[idx].header->di->fpath = single_file;
           macro_matrix[idx].header->di->value = macro_value;
           macro_matrix[idx].header->di->ln    = line_number;
           macro_matrix[idx].header->di->next  = NULL;

           macro_matrix[idx].header->next = NULL;
       } else {
           append_define_info_into_linker(macro_matrix[idx].header,
                                          rtrim(macro_mname),single_file,macro_value == NULL?NULL:rtrim(macro_value),line_number);
       }

       g_macro_nums++;

    } /* end while(fgets(...)) */

    fclose(fd);

}

static char append_define_info_into_linker(MACRO_INFO_NODE * pheader,   /* in, the target linker's header */
                                           char * macro_name,           /* in, macro name */
                                           char * hostfile,             /* in, macro in which file */
                                           char * value,                /* in, macro value, can be NULL */
                                           unsigned int line_number)    /* in, the line number of the macro in the hostfile */
{
    MACRO_INFO_NODE * pcursor = pheader;
    MACRO_INFO_NODE * pinspos = pheader;
    MACRO_INFO_NODE  * newnode_min = NULL;

    DEFINE_INFO_NODE * newnode_din = NULL;
    DEFINE_INFO_NODE * din_cursor  = NULL;
    DEFINE_INFO_NODE * din_inspos  = NULL;

    char ret = 0;

    assert(pheader    != NULL);
    assert(macro_name != NULL);
    assert(hostfile  != NULL);

    while (pcursor != NULL && strcmp(pcursor->name, macro_name) != 0) {
        pinspos = pcursor;
        pcursor = pcursor->next;
    }

    if (pcursor == NULL) {
        /* we are in the end of linker... */
        newnode_min = (MACRO_INFO_NODE *)malloc(sizeof(MACRO_INFO_NODE));
        if (newnode_min == NULL) {
            assert(1);
        }

        strncpy(newnode_min->name,macro_name,MAX_MACRO_NAME_LEN);

        newnode_min->di = (DEFINE_INFO_NODE *)malloc(sizeof(DEFINE_INFO_NODE));
        if (newnode_min->di == NULL) {
            assert(1);
        }

        newnode_min->di->fpath = hostfile;
        newnode_min->di->ln    = line_number;
        newnode_min->di->value = value;
        newnode_min->di->next  = NULL;

        newnode_min->fi   = NULL;
        newnode_min->next = NULL;

        pinspos->next = newnode_min;

    } else {

        if (pcursor->di == NULL) {
            /* we get a matched item,but its di is empty */
            newnode_din = (DEFINE_INFO_NODE *)malloc(sizeof(DEFINE_INFO_NODE));
            if (newnode_din == NULL) {
                assert(1);
            }

            newnode_din->fpath = hostfile;
            newnode_din->value = value;
            newnode_din->ln    = line_number;
            newnode_din->next  = NULL;

            pcursor->di = newnode_din;

        } else {

            din_cursor = pcursor->di;
            /* we already have the macro in the linker. trying to insert define infor into the pcursor's di linker */
            while (din_cursor != NULL &&
                   (din_cursor->fpath != hostfile || din_cursor->ln != line_number || (din_cursor->value != NULL && din_cursor->value == value) ))
            {
                din_inspos = din_cursor;
                din_cursor = din_cursor->next;
            }

            if (din_cursor == NULL) {
                /* we are in the end of linker... */
                newnode_din = (DEFINE_INFO_NODE *)malloc(sizeof(DEFINE_INFO_NODE));
                if (newnode_din == NULL) {
                    assert(1);
                }

                newnode_din->fpath = hostfile;
                newnode_din->value = value;
                newnode_din->ln    = line_number;
                newnode_din->next  = NULL;

                din_inspos->next = newnode_din;
           } else {
                ret = 1;
           }
        }
    }

    return ret;
}


static char * rtrim(char *str)
{
    char *ptr;
    int   len;

    len = strlen(str);
    for (ptr = str + len - 1; ptr >= str && isspace((int)*ptr ); --ptr);

    ptr[1] = '\0';

    return str;
}

static char * ltrim(char *str)
{
    char *ptr;
    int  len;

    for (ptr = str; *ptr && isspace((int)*ptr); ++ptr);

    len = strlen(ptr);
    memmove(str, ptr, len + 1);

    return str;
}
