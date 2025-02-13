#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>


FILE* FD;
int SIG = 0xAABBCCDD;

char X[256][256];

void pack_dir(char* dnme) {	
	printf("%s \n", dnme);
	struct dirent* dp;
	DIR* dfd = opendir(dnme);
	if(!dfd) return fprintf(stderr, "Can't open %s\n", dnme);
	while ((dp = readdir(dfd)) != NULL) {
		char* nme = dp->d_name;
		if(!strcmp(".", nme) || !strcmp("..", nme) ) continue;
		printf("%s %d\n", nme, dp->d_type);
		
		// new names
		char nnme[255];
		for(int i = 0; i < 256; i++) nnme[i] = 0;
		strcat(nnme, dnme); 
		strcat(nnme, "/");
		strcat(nnme, nme);
		if(dp->d_type == DT_DIR) { 
			pack_dir(nnme);
			continue;
		} 
		FILE* nfile = fopen(nnme, "rb");
		if(!nfile) { printf("file read error: %s \n", nnme); continue; }
		fseek(nfile, 0, SEEK_END);
		int len = ftell(nfile); 
		if(len&3) printf("unaligned len %d \n", len);
		len = len&3 ? (len&-4)+4 : len;
		printf("len %X \n", len);
		int slen = strlen(nme);
		char trunc_nme[16] = "";
		strncpy(trunc_nme, nme, slen>16 ? 16 : slen );
		fseek(nfile, 0, SEEK_SET);
		// writing data
		fwrite(&SIG, 4, 1, FD);				// 4byte signature
		fwrite(&trunc_nme, 1, 16, FD);	// 16byte name
		fwrite(&len, 4, 1, FD);				// 4byte data length
		char* data = malloc(len);
		fread(data, 1, len, nfile);
		fwrite(data, 1, len, FD);			// data
		free(data);
	}
}

void main(int argc, char* argv[]) {
	printf("RES PACKER PC32\n");
	if(argc < 2) { return printf("usage respack directory"); }

	char* dir = argc < 2 ? "test" : argv[1];
	FD = fopen("data.res", "wb");
	if(!FD) return printf("couldnt open data.res!\n"); 
	pack_dir(dir);
	fclose(FD);
}


