#include <dirent.h> 
#include <stdio.h> 
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include <time.h>
#include <pthread.h>

//-------------  Macros from the original code ---------------------------
#define BUFFER_SIZE 1048576 // 1MB


//-------------  Macros that I added ---------------------------
#define MAX_FILES 1000
#define MAX_LOAD 20
#define MAX_ZIP 20


//------------- Global vars from the original code -------------

char **files = NULL;
FILE *f_out; 
int total_in = 0, total_out = 0;  
int nfiles = 0;




//-------------  Global vars that I added ---------------------------

char* full_path_arr[MAX_FILES];
char* argv_1 = NULL;

typedef unsigned char buffer_t[BUFFER_SIZE];
buffer_t buffer_in_arr[MAX_FILES];
buffer_t buffer_out_arr[MAX_FILES];

int nbytes_arr[MAX_FILES];
int nbytes_zipped_arr[MAX_FILES];

int written[MAX_FILES];


// Shared between Load, zip and/or save 


// ---------------- Load Globals ----------------
int loaded_files = 0;



//---------------- Zip Globals ----------------
int zipped_files = 0;

// ---------------- Save Globals ----------------
int saved_files = 0;


// ---------------- Load - Zip  ----------------------

// Global vars
int LZ_buffer[MAX_LOAD];

int LZ_count = 0;
int LZ_fill = 0;
int LZ_use = 0;

pthread_mutex_t lock_LZ = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t  LZ_empty = PTHREAD_COND_INITIALIZER;
pthread_cond_t  LZ_full = PTHREAD_COND_INITIALIZER;

// Load Put
void Lput(int value) 
{
    LZ_buffer[LZ_fill] = value;
    LZ_fill = (LZ_fill + 1) % MAX_LOAD; 
    LZ_count++;
}

// Zip Get 
int Zget() 
{
    int tmp = LZ_buffer[LZ_use];
    LZ_use = (LZ_use + 1) % MAX_LOAD;  
    LZ_count--;
    return tmp;
}


// ---------------- Zip - Save  ----------------------

// Global vars
int ZS_buffer[MAX_ZIP];

int ZS_fill = 0;
int ZS_count = 0;
int ZS_use = 0;

pthread_mutex_t lock_ZS = PTHREAD_MUTEX_INITIALIZER;

pthread_cond_t  ZS_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t  ZS_empty = PTHREAD_COND_INITIALIZER;


// Zip Put
void Zput(int value) 
{
    ZS_buffer[ZS_fill] = value;
    ZS_fill = (ZS_fill + 1) % MAX_ZIP; 
    ZS_count++;
}

// Save Get
int Sget() 
{
    int tmp = ZS_buffer[ZS_use];
    ZS_use = (ZS_use + 1) % MAX_ZIP;
    ZS_count--;
    return tmp;

}


// ---------------- LOAD ----------------------
void Zproducer_load()
{
    while (loaded_files < nfiles)        
        { 

            pthread_mutex_lock(&lock_LZ);

                while (LZ_count == MAX_LOAD)
                {
                    pthread_cond_wait(&LZ_empty, &lock_LZ);
                } 
                
            
                int i = loaded_files;
                loaded_files += 1;     

                int len = strlen(argv_1) + strlen(files[i]) + 2;
            
                char *full_path = malloc(len*sizeof(char));
                assert(full_path_arr != NULL);
                full_path_arr[i] = full_path;

                
            pthread_mutex_unlock(&lock_LZ);
            
            strcpy(full_path_arr[i], argv_1);
            strcat(full_path_arr[i], "/");
            strcat(full_path_arr[i], files[i]);

            
            unsigned char buffer_in[BUFFER_SIZE];
            unsigned char buffer_out[BUFFER_SIZE];
            

            // load file
            FILE *f_in = fopen(full_path_arr[i], "r");
            assert(f_in != NULL);
            int nbytes = fread(buffer_in, sizeof(unsigned char), BUFFER_SIZE, f_in);
            nbytes_arr[i] = nbytes;
            fclose(f_in);

            memcpy(buffer_in_arr[i], buffer_in, BUFFER_SIZE);
            memcpy(buffer_out_arr[i], buffer_out, BUFFER_SIZE);



            //now acquire the load lock and increement the number of loaded files
            pthread_mutex_lock(&lock_LZ);

            
                // update the global vars
                total_in += nbytes;
                
                // Do the producer job
                Lput(i);
                pthread_cond_broadcast(&LZ_full);

            pthread_mutex_unlock(&lock_LZ);

        }
}

// ---------------- ZIP ----------------------
void Lconsumer_Sproducer_zip()
{
    
    while (zipped_files < nfiles)
    {
            pthread_mutex_lock(&lock_LZ);

                while (LZ_count == 0)
                {
                    pthread_cond_wait(&LZ_full, &lock_LZ);
                }

                int i = zipped_files;
                zipped_files += 1;

                int tmp = Zget();
                pthread_cond_broadcast(&LZ_empty); 
                

        pthread_mutex_unlock(&lock_LZ);
        
        z_stream strm;
		int ret = deflateInit(&strm, 9);
		assert(ret == Z_OK);
		strm.avail_in = nbytes_arr[i];
		strm.next_in = buffer_in_arr[i];
		strm.avail_out = BUFFER_SIZE;
		strm.next_out = buffer_out_arr[i];

		ret = deflate(&strm, Z_FINISH);
		assert(ret == Z_STREAM_END);

        int nbytes_zipped = BUFFER_SIZE-strm.avail_out;
        nbytes_zipped_arr[i] = nbytes_zipped;


        // -------- Produce ----------
        pthread_mutex_lock(&lock_ZS);


            while (ZS_count == MAX_ZIP)
            {
                pthread_cond_wait(&ZS_empty, &lock_ZS);
            }
            
            // do the producer job

            Zput(i);
            written[i] = 1;

            pthread_cond_broadcast(&ZS_full);


        // release the producer lock
        pthread_mutex_unlock(&lock_ZS);

        

    }

}

// ---------------- SAVE ----------------------
void Zconsumer_save()
{
    while (saved_files < nfiles)
    {

        pthread_mutex_lock(&lock_ZS);
            while (ZS_count == 0) 
            {
                pthread_cond_wait(&ZS_full, &lock_ZS);
            }
                    
            int i = saved_files;
            saved_files += 1;

            while (written[i] == 0)
            {
                pthread_cond_wait(&ZS_full, &lock_ZS);
            }

            // do the consumer job
            int tmp = Sget();
            pthread_cond_broadcast(&ZS_empty); 

            fwrite(&nbytes_zipped_arr[i], sizeof(int), 1, f_out);
            fwrite(buffer_out_arr[i], sizeof(unsigned char), nbytes_zipped_arr[i], f_out);

            // Update the global vars
            total_out += nbytes_zipped_arr[i];
                
        // release the consumer lock 
        pthread_mutex_unlock(&lock_ZS);  

    }
		
}


int cmp(const void *a, const void *b) {
	return strcmp(*(char **) a, *(char **) b);
}

int main(int argc, char **argv) {
	// time computation header
	struct timespec start, end;
	clock_gettime(CLOCK_MONOTONIC, &start);
	// end of time computation header

	// do not modify the main function before this point!

	assert(argc == 2);

	DIR *d;
	struct dirent *dir;
    // char **files = NULL;  // made global 
	// int nfiles = 0;

	d = opendir(argv[1]);
	if(d == NULL) {
		printf("An error has occurred\n");
		return 0;
	}

	// create sorted list of PPM files
	while ((dir = readdir(d)) != NULL) {
		files = realloc(files, (nfiles+1)*sizeof(char *));
		assert(files != NULL);

		int len = strlen(dir->d_name);
		if(dir->d_name[len-4] == '.' && dir->d_name[len-3] == 'p' && dir->d_name[len-2] == 'p' && dir->d_name[len-1] == 'm') {
			files[nfiles] = strdup(dir->d_name);
			assert(files[nfiles] != NULL);

			nfiles++;
		}
	}
	closedir(d);
	qsort(files, nfiles, sizeof(char *), cmp);

	// create a single zipped package with all PPM files in lexicographical order
	// int total_in = 0, total_out = 0;  


    // ------------------ Handling some Global vars before creating threads ------------------
    argv_1 = argv[1];
    f_out = fopen("video.vzip", "w");
    assert(f_out != NULL); 
	

	//for(int i=0; i < nfiles; i++)
    //{

        /* // Moved to LOAD

        int len = strlen(argv[1])+strlen(files[i])+2;
		char *full_path = malloc(len*sizeof(char));
		assert(full_path != NULL);
		strcpy(full_path, argv[1]);
		strcat(full_path, "/");
		strcat(full_path, files[i]);

        unsigned char buffer_in[BUFFER_SIZE];
		unsigned char buffer_out[BUFFER_SIZE];

        // load file
		
		assert(f_in != NULL);
		int nbytes = fread(buffer_in, sizeof(unsigned char), BUFFER_SIZE, f_in);
		fclose(f_in);
		total_in += nbytes;

        
        
        */





       /*
       	// MOved to ZIP

        // zip file
		z_stream strm;
		int ret = deflateInit(&strm, 9);
		assert(ret == Z_OK);
		strm.avail_in = nbytes;
		strm.next_in = buffer_in;
		strm.avail_out = BUFFER_SIZE;
		strm.next_out = buffer_out;

		ret = deflate(&strm, Z_FINISH);
		assert(ret == Z_STREAM_END);


        int nbytes_zipped = BUFFER_SIZE-strm.avail_out;

       */


        /* // Moved to save


        // dump zipped file
        fwrite(&nbytes_zipped, sizeof(int), 1, f_out);
		fwrite(buffer_out, sizeof(unsigned char), nbytes_zipped, f_out);
		total_out += nbytes_zipped;
        
        
        */
		
		


		// free(full_path);
	//}


    // Creating threadsa
    int num_load_threads = 1;
    int num_zip_threads = 15;
    int num_save_threads = 1;

    pthread_t load_threads[num_load_threads];
    pthread_t zip_threads[num_zip_threads];
    pthread_t save_threads[num_save_threads];



    // create save threads
    for(int i = 0; i < num_save_threads; i++)
    {
        if (pthread_create(&save_threads[i], NULL,  (void* (*)(void*))Zconsumer_save, NULL) != 0)
        {
            printf("Error in SAVE creating thread \n");
            exit(1);
        }
    }

    // create zip threads
    for(int i = 0; i < num_zip_threads; i++)
    {
        if (pthread_create(&zip_threads[i], NULL,  (void* (*)(void*))Lconsumer_Sproducer_zip, NULL) != 0)
        {
            printf("Error in ZIP creating thread \n");
            exit(1);
        }
    }


    // create load threads
    for(int i = 0; i < num_load_threads; i++)
    {
        if (pthread_create(&load_threads[i], NULL,  (void* (*)(void*))Zproducer_load, NULL) != 0)
        {
            printf("Error in LOAD creating thread \n");
            exit(1);
        }
    }

    
    // join loads
    for (int i = 0; i < num_load_threads; i++)
    {
        pthread_join(load_threads[i], NULL);
    }

    // join zips
    for (int i = 0; i < num_zip_threads; i++)
    {
        pthread_join(zip_threads[i], NULL);
    }

    // join saves
    for (int i = 0; i < num_save_threads; i++)
    {
        pthread_join(save_threads[i], NULL);
    }


	fclose(f_out);

	printf("Compression rate: %.2lf%%\n", 100.0*(total_in-total_out)/total_in);

	// release list of files
	for(int i=0; i < nfiles; i++)
		free(files[i]);
	free(files);

	// do not modify the main function after this point!

	// time computation footer
	clock_gettime(CLOCK_MONOTONIC, &end);
	printf("Time: %.2f seconds\n", ((double)end.tv_sec+1.0e-9*end.tv_nsec)-((double)start.tv_sec+1.0e-9*start.tv_nsec));
	// end of time computation footer

	return 0;
}