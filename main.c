#include <GL/freeglut.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

//#include <pthread.h>


char* romstr;
int FLAG_PSRAM; int FLAG_WINDOWED;


/*
	SPECS:
	32bit MISC architecture
	63 core
	16MB memory word addressed unless in byte mode

	MEM MAP(IN WORDS):
		0x00000000-0x0000FFFF 64KW/256KB SRAM 4KB per core x 63 cores + 4KB MREGS
		0x00010000-0x0001FFFF 64KW/256KB General Purpose SRAM, CORE0 can execute here as well
		0x00020000-0x000FFFFF 4MB/1MW		Saved Storage source code etc
		0x00100000-0x00FFFFFF 60MB/15MW	SDRAM non saved
		64MB/16MW total memory space

	DMA:
	ALWAYS from MEM to SRAM or from SRAM to SRAM, never MEM to MEM
	BANK 0-1 in OPT is SRAM(done at 4words per cycle), otherwise 2+ cycles per word
	After transfer is done cnt register is set to 0

	IOBASE and Peripherals currently unused, asides from UART0
	FE18 UART TX (printf)
	FE19 UART RX (getchar)
*/

// MEM MAP
#define RESETS		0xFF00	//4words reset pulldn
#define DMABASE	0xFC00	//64*4words 0OPT, 1TO, 2FROM, 3CNT
#define DMACNT 3
#define DMAFROM 2
#define DMATO 1
#define KBASE		0xFD00	//16words(input peripherals)
#define ABASE		0xFD10	//34words(BG, OPT ,16chX2)
#define VBASE		0xFE00	//(0FBbank#, 1PAL#, 2MODE, 3RESX, 4RESY, 5POLL(read))
#define FBUF		(MEM+MEM[VBASE])
#define IWIDTH		(MEM[VBASE+3])
#define IHEIGHT	(MEM[VBASE+4])
#define IOBASE		0xFE10	//(0..7 GPIO|DIR, 8UART0_TX, 9UART0_RX)
#define SAVBASE	0xFF10
#define SLEEPBASE 0xFF14
#define TIMEBASE  0xFF15

// Instruction macro
#define xNOP	0
#define xRET	1
#define xJMP	2
#define xJZ		3
#define xJ1		4
#define xNEXT	5
#define xSHL	6
#define xSHR	7
#define xSMD	8
#define xLIT	9
#define xEXE	10
#define xPUSH	11
#define xPOP	12
#define xSTORE	13
#define xFETCH	14
#define xIREG	15
#define xAREG	16
#define xBREG	17
#define xSTOA	18
#define xSTOB	19
#define xFAP	20
#define xSBP	21
#define xDUP	22
#define xDRP	23
#define xSWP	24
#define xOVR	25
#define xADD	26
#define xAND	27
#define xEOR	28
#define xNOT	29
#define xNEG	30
#define xMUL	31


typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned short ushort;
#define MAXMEM 131072
#define MAXEXTMEM 8364032
#define XWRAP 0x3FF
#define IWRAP 0x7FFFFFFF
#define XSRAM 0x7FFFF
#define MDELAYCNT 3


// Memory

// 512KB SRAM + 3.5MB savedRAM + 60MB resources
int MEM[16*1024*1024] __attribute__((aligned(32)));  
int MEMROM[128*1024];

// Threading
//pthread_t mthread[62];
//pthread_t dma_thread;
//pthread_t sound_thread;

// MISC
int DBGSTEP = 0;
uint64_t GCLK; // global clock
double GTIME;  // global time
char*  GMSG[64];
int dummy;

// OGL
uint CLOSING = 0;
int WIDTH, HEIGHT;

// Stack lengths
#define DLEN	15
#define RLEN	31
struct M63dat {
	uint CLK;
	uint P;
	uint WRAP;			// 1K words(2KB) for slave, 64K(128KB) words for master
	int I;				// Sign allows for rel jmp
	ushort slot;		// 6 slots
	uchar TI;			// Top index
	uchar RI;			// Rtop index
	int DSTK[DLEN+1];	// Data Stack
	int RSTK[RLEN+1];	// Return Stack
	int A;				// A register for reads
	int B;				// B register for writes
	short STAT;			// Machine's current instruction set index(-1 is finalize)
} machine[65];

// FN declaration for clang
void Mrun(ushort ID);

// M32 MACROS
#define MADDR (ID<<10)
#define MP (M->P&M->WRAP)									// P
#define MPP M->P = (M->P+1)&M->WRAP;					// P+1
#define MPR M->P = (M->P+(M->I>>5)) & M->WRAP;		// P+rel
#define INXT M->I = MEM[MP+MADDR]; M->slot = 1; return;
#define ICNT M->I >>=5; M->slot = (M->slot+1)&3;

#define PUSH(X) M->TI = (M->TI+1)&DLEN; M->DSTK[M->TI]=X;
#define POP M->DSTK[M->TI]; M->TI = (M->TI-1)&DLEN;
#define RPUSH(X) M->RI = (M->RI+1)&RLEN; M->RSTK[M->RI]=X;
#define RPOP M->RSTK[M->RI]; M->RI = (M->RI-1)&RLEN;

#define TT M->DSTK[M->TI]
#define ST M->DSTK[(M->TI-1)&DLEN]
#define RT M->RSTK[M->RI]		// I
#define RN M->RSTK[M->RI-1]	// 2nd rstack
#define R3 M->RSTK[M->RI-2]	// 3rd rstack
#define R4 M->RSTK[M->RI-3]	// 4th rstack
#define R5 M->RSTK[M->RI-4]	// 5th rstack

#define DUP TMPA = TT; PUSH(TMPA);
#define DRP dummy = POP;
#define RDRP dummy = RPOP;

#define DELAYM0 TMPA = MDELAYCNT; if(!ID) { while(TMPA) { GCLK++; TMPA = MEM_DELAY(TT, TMPA); } }

uint ctimeu () {
	struct timeval te;
	gettimeofday(&te, NULL);
	return te.tv_usec;
}


// MEMORY ACCESS
int MEM_DELAY(int addr, int delay) {
	if(addr > XSRAM && delay) return delay-1;
	return 0;
}

void MEM_STORE(uint addr, int val, int ID) {
	if(addr == 0xFFFF) return;
	if(addr == 0xFFFE) { COLD(); M16b(0); }
	if(addr == SAVBASE && val == 0xCFCF) {	// SAVE PSRAM
		MEMROM[20] = MEM[20];
		FILE* h = fopen("BLK.ROM", "wb");
		fwrite(MEMROM, 512*1024, 1, h);
		fwrite(MEM+0x20000, 0x400000, 1, h);

		//FILE* h = fopen("PSRAM.bin", "wb");
		//fwrite(MEM+0x20000, 0x800000, 1, h);

		fflush(h);
		fclose(h);
	
		return;
	}
	if(addr == SLEEPBASE) { usleep(val); return; }
	if(addr == VBASE) {
		MEM[VBASE] = val;
		glDrawPixels(IWIDTH, IHEIGHT, GL_RGB, 0x8362, MEM+val);
		glutSwapBuffers();
		return;
	}
	if(addr == IOBASE+8) { printf("%c", (char)val); return; } // UART0_TX
	if(addr >= 0xFC00 && addr <= 0xFFFF) { MEM[addr] = val; return; } // MREGS
	addr = ID ? (addr&XWRAP)+(ID<<10) : addr;
	if(machine[ID].STAT == 3) {
		((char*)MEM)[addr] = val;
		return;
	} MEM[addr] = val; return;
}

int MEM_FETCH(uint addr, int ID) {
	if(addr == 0xFFFF) return ID;
	if(addr == VBASE+5) glutMainLoopEvent();	
	if(addr == IOBASE+9) return getchar();	// UART0 RX
	if(addr == TIMEBASE) return ctimeu();
	if(addr >= 0xFC00 && addr <= 0xFFFF) return MEM[addr]; // MREGS
	addr = ID ? (addr&XWRAP)+(ID<<10) : addr; // M0 can read all
	if(machine[ID].STAT == 3) {
		return ((char*)MEM)[addr]&0xFF;
	} return MEM[addr];
}

uint sigres(ushort ID) { return MEM[RESETS+(ID>>4)]&(1<<(ID&15)); }

void Mreset(ushort ID) {
	machine[ID].P = 0;
	machine[ID].WRAP = ID ? 0x3FF : 0x1FFFF;
	machine[ID].slot = 0;
	machine[ID].STAT = 0;
	machine[ID].TI = DLEN;
	machine[ID].RI = RLEN;
}

char* INST[32] = { "NOP", "RET", "JMP", "JZ", "J1", "NEXT", "SHL", "SHR", "SMD", "LIT",
"EXE", "PUSH", "POP", "!", "@", "I", "A", "B",  "!A", "!B", "A@+", "B!+",
"DUP", "DRP", "SWP", "OVR", "ADD", "AND", "OR", "NOT", "NEG", "MUL" };


void dbgi(int ID) {
	struct M63dat* M = &machine[ID];
	printf("M%d P%X S%d %s %X %X R:%X A:%X B:%X\n",
				ID, MP+MADDR, M->slot, INST[M->I&31], TT, ST, RT, M->A, M->B);
}

void pchar(char* ch, int cnt) {
	for(int i = 0; i < cnt; i++) printf("%c", ch[i] != 0? ch[i]:"." );
	printf("\n");
}

void printnme(char* tag, int addr) {
	printf("%s ", tag);
	char* N = MEM+addr;
	N -= 12;
	pchar(N, 12);
}

void dostats(uint ID) {
	struct M63dat* M = &machine[ID];
	printf("S%d  P:%X  %d[%X %X] R%d[%X %X %X %X %X] A:%X B:%X\n", 
			M->STAT, M->P, M->TI, TT, ST, M->RI, RT, RN, R3, R4, R5, M->A, M->B);
	char* tchar = MEM+TT;
	printf("WORD: "); pchar(((char*)MEM)+32, 16);
	printf("NME: "); pchar(tchar-12, 12);

	int nxti = MEM[M->P+1];
	printf("NXTI: %X\n", nxti);
	/*
	printf("A: "); 
		for(int i = 0; i<20; i++) printf("%c", ((char*)MEM)[M->A+i]);
		printf("\n");
	*/
}

void phex(int addr, int cnt) {
	printf("%X ", addr);
	for(int i = 0; i < cnt; i++)
		printf("%X ", MEM[addr+i]);
	printf("\n");
}

void dobreak(uint ID) {
	printf("BRK "); dostats(ID);
	char x = getchar();
	if(x == 'x') DBGSTEP = 1;
	if(x == 'd') {
		struct M63dat* M = &machine[ID];
		printf("memdump "); phex(M->P, 8);
	}
	usleep(5000);
}

void finalize(uint ID) {
	MEM[RESETS+(ID<<4)] &= ~(1<<(ID&15)); // pull reset low
	printf("EXIT %d \n", ID);
	if(!ID) {
		printf("GCLK %d\n", GCLK);
	} exit(0);
}

void M16b(ushort ID) {
	register int TMPA, TMPB, MI;
	register uint TMPU;
	struct M63dat* M = &machine[ID];
again:
	if(M->slot == 0) { INXT; return; }
	// CALL
	if(M->slot == 1 && M->I < 0) {
		RPUSH((MP)+1);
		TMPA = IWRAP;
		TMPB = M->WRAP;
		M->P = M->I&IWRAP&M->WRAP;
		//printnme("CALL",M->P);
		INXT;
	}
	if(M->slot == 1) M->I = (M->I<<1)>>1; // sign extend
	MI = M->I;

	if(DBGSTEP) {
		dbgi(ID);
		char x = getchar();
		if(x == 'x') DBGSTEP = 0;
	}

	//dbgi(ID);

	switch(M->I&31) {
		// REST ------------------------
		case xNOP:
			MPP; INXT;
		case xRET:
			M->P = RT;
			RDRP;
			INXT;
		case xJMP:
			MPR;
			//printnme("JMP", M->P);
			INXT;
		case xJZ:
			if(TT>0) { MPP; INXT; }
			MPR; INXT;
		case xJ1:
			if(TT>0) { MPR; INXT; }
			MPP; INXT;
		case xNEXT:
			if(RT > 1) {
				RT--; MPR; INXT;
			} RDRP; MPP; INXT; break;
		case xSHL:
			TT <<=abs(MI>>5);
			MPP; INXT;
		case xSHR:
			TMPB = TT;
			TMPU = ((uint)TMPB)>>abs(MI>>5); // signed shift
			TMPA = TT>>abs(MI>>5); // unsigned shift
			TT = (MI>>5)>0 ? TMPU : TMPA;
			MPP; INXT;
		case xSMD:
			switch(MI>>5) {
				case -3: { dostats(ID); break; }
				case -2: { dobreak(ID); break; }
				case -1: { dostats(ID); finalize(ID); return; }
				default:
					M->STAT = MI>>5;
					break;
			} MPP; INXT; return;
		// SPEC ----------------------------
		case xLIT:
			MPP;
			PUSH(MEM[MP+MADDR]);
			break;
		// NOSC ----------------------------
		case xEXE:
			RPUSH((MP)+1);
			TMPA = POP;
			M->P = TMPA&M->WRAP;
			//printnme("EXE",M->P);
			//printf("GCLK %d \n", GCLK);
			INXT;
		case xPUSH:
			RPUSH(TT);
			DRP;
			break;
		case xPOP:
			PUSH(RPOP);
			break;
		case xSTORE:
			//DELAYM0;
			TMPA = POP;
			TMPB = POP;
			MEM_STORE(TMPA, TMPB, ID);
			PUSH(TMPA);
			break;
		case xFETCH:
			//DELAYM0;
			TMPA = TT;
			TT = MEM_FETCH(TMPA, ID);
			break;
		case xIREG:
			PUSH(RT);
			break;
		case xAREG:
			PUSH(M->A);
			break;
		case xBREG:
			PUSH(M->B);
			break;
		case xSTOA:
			M->A = POP;
			break;
		case xSTOB:
			M->B = POP;
			break;
		case xFAP:
			//DELAYM0;
			PUSH(MEM_FETCH(M->A, ID));
			M->A++;
			break;
		case xSBP:
			//DELAYM0;
			TMPA = POP;
			MEM_STORE(M->B, TMPA, ID);
			M->B++;
			break;
		case xDUP:
			DUP;
			break;
		case xDRP:
			DRP;
			break;
		case xSWP:
			TMPA = TT;
			TMPB = ST;
			TT = TMPB;
			ST = TMPA;
			break;
		case xOVR:
			TMPA = ST;
			PUSH(TMPA);
			break;
		case xADD:
			TMPA = POP;
			TT += TMPA;
			break;
		case xAND:
			TMPA = POP;
			TT &= TMPA;
			break;
		case xEOR:
			TMPA = POP;
			TT ^= TMPA;
			break;
		case xNOT:
			TT = ~TT;
			break;
		case xNEG:
			TT *= -1;
			break;
		case xMUL:
			TMPA = POP;
			TT *= TMPA;
			break;
	}
	M->I >>=5;
	M->slot = (M->slot+1)&7;
	if(M->slot == 7) { MPP; INXT; M->slot = 0; }
	goto again;
}

void MSIMD(ushort ID) { }

void MGPU(ushort ID) {
	// DIV
	// SQRT
	// RCP
	// LERP
	// REGISTER
}

uint DFROM, DTO, DCNT, DIDX, DSTEP, SDELAY;

// FAST DMA 4 16bit words per cycle
void FDMA_STEP(int delay) {
	if(SDELAY) { SDELAY--; return; }
	SDELAY = delay;
	if(DCNT>=4) {
		MEM[DTO+DSTEP]	  = MEM[DFROM+DSTEP];
		MEM[DTO+DSTEP+1] = MEM[DFROM+DSTEP+1];
		MEM[DTO+DSTEP+2] = MEM[DFROM+DSTEP+2];
		MEM[DTO+DSTEP+3] = MEM[DFROM+DSTEP+3];
		DCNT-=4; DSTEP+=4;
		MEM[DMABASE+(DIDX<<2)+DMACNT] -= 4;
	} else {
		MEM[DTO+DSTEP] = MEM[DFROM+DSTEP];
		MEM[DMABASE+(DIDX<<2)+DMACNT] -= 1;
		DCNT--; DSTEP++;
	}
}

// SLOW DMA 1 word per 16 cycle(maybe even slower)
void SDMA_STEP(int delay) {
	if(SDELAY) { SDELAY--; return; }
	int X = MEM[DFROM+DSTEP];
	MEM[DTO+DSTEP] = MEM[DFROM+DSTEP];
	DCNT--; DSTEP++; SDELAY = delay;
	MEM[DMABASE+(DIDX<<2)+DMACNT] -= 1;
	// printf("DCNT:%d\n", DCNT);
}

void MDMA() {
	while(1) {
	// RECT DMA
	// Standard DMA
	if(DCNT && !(DFROM>>15)) { FDMA_STEP(0); return; }
	if(DCNT && (DFROM>>15))  { SDMA_STEP(16); return; }
	// next machine's DMA
	DIDX = (DIDX+1)%63;
	DFROM =	(MEM[DMABASE+(DIDX<<2)]&0xFF)<<15|MEM[DMABASE+(DIDX<<2)+DMAFROM];
	DTO   =	MEM[DMABASE+(DIDX<<2)+DMATO];
	DCNT  =	MEM[DMABASE+(DIDX<<2)+DMACNT];
	DSTEP  =  0;
	if(DCNT) printf("DMA ID:%d FROM:%X TO:%X CNT:%d\n", DIDX, DFROM, DTO, DCNT);
	}
}

void Mrun(ushort ID) {
	while(1) {
		if(!ID) {
			if(CLOSING) finalize(0);
			GCLK++;
			//MDMA();
		}
		if(!sigres(ID)) { Mreset(ID); usleep(100); continue; }
		machine[ID].CLK = GCLK;

		switch(machine[ID].STAT) {
			case 1:  { MSIMD(ID); break; }
			case 2:  { MGPU(ID); break;}
			default: { M16b(ID); break; }
		}
	}
}

void resize(int W, int H) {
	WIDTH = W; HEIGHT = H;
	glPixelZoom((float)W/(float)IWIDTH, (float)H/(float)IHEIGHT);
	glViewport(0, 0, IWIDTH, IHEIGHT);
}

void char_cb(uchar ch, int x, int y) {
	//printf("char callback %c\n", unicode);
	if(ch < 32 || ch > 126) { MEM[KBASE+1] = ch+200; return; }
	MEM[KBASE] = ch;
}

void key_cb(int key, int x, int y) {
	//printf("keyboard callback: action%d mods%d key%c \n", action, mods, key);
	if(key == 112 || key == 114) {MEM[KBASE+2] = key; return; }
	MEM[KBASE+1] = key;
}

void mouse_cb(int btn, int state, int x, int y) {
	int mx = abs((int)(((float)x/(float)WIDTH)*((float)IWIDTH)));
	int my = abs(IHEIGHT - (int)(((float)y/(float)HEIGHT)*((float)IHEIGHT)));
	MEM[KBASE+3] = (ushort)mx;
	MEM[KBASE+4] = (ushort)my;
	MEM[KBASE+5] = ((uchar)btn<<8)|state&0xF;
}

void mousescr_cb(double x, double y) {
	MEM[KBASE+6] = (uchar)y, (uchar)x;
}

void joystick() {

}

void Msound() {
}

void gldummy() { glutPostRedisplay(); }
void glnothing() { }


void extra_init() {
	IWIDTH = 640;
	IHEIGHT = 480;
   IWIDTH = 640;
   IHEIGHT = 480;

   glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_GLUTMAINLOOP_RETURNS);
   glutInitDisplayMode(GLUT_DEPTH | GLUT_DOUBLE | GLUT_RGBA);
   int sx = glutGet(GLUT_SCREEN_WIDTH);
   int sy = glutGet(GLUT_SCREEN_HEIGHT);
	glutInitWindowSize(800, 600);
	if(!FLAG_WINDOWED) {
		glutInitWindowSize(sx, sy-25);
		glutInitWindowPosition(0, 0);
		glutInitDisplayMode(GLUT_DEPTH | GLUT_DOUBLE | GLUT_RGBA | GLUT_BORDERLESS);
	}
   int window = glutCreateWindow("PC32X");
   glutReshapeFunc(resize);
   glutKeyboardFunc(char_cb);
   glutSpecialFunc(key_cb);
   glutMouseFunc(mouse_cb);

   glutDisplayFunc(glnothing);
   glutIdleFunc(gldummy);

   // joystick

   //sound init

}


void COLD() {	
   // load BOOT ROM
   FILE* handle = fopen(romstr, "rb");
   fseek(handle, 0, SEEK_END);
   int sze = ftell(handle);
   sze = sze > MAXEXTMEM? MAXEXTMEM : sze;
   fseek(handle, 0, SEEK_SET);
   fread(MEM, sze, 1, handle);
	memcpy(&MEMROM, &MEM, 512*1024);
   fclose(handle);
	printf("ROM OK %d bytes \n", sze);
	//MEM[20] = MEM[0x20000];
	
	// load resources
   handle = fopen("data.res", "rb");
   if(handle) { 
		fseek(handle, 0, SEEK_END);
		sze = ftell(handle);
		fseek(handle, 0, SEEK_SET);
		printf("loading data.res %d bytes \n", sze); 
		fread(MEM+0x100000, sze, 1, handle); 
	}

   // reset all machines, run M0
   for(int i = 0; i < 63; i++)
      Mreset(i);
   MEM[RESETS]   = 1;
   MEM[RESETS+1] = 0;   // all in reset except M0
   MEM[RESETS+2] = 0;
   MEM[RESETS+3] = 0;
	printf("RESETS OK %d Core(s) Available \n", 1);

   MEM[VBASE] = 0;

   GCLK = 0;
   DFROM = 0; DTO = 0; DCNT = 0; DIDX = 0; DSTEP = 0; SDELAY = 0;
   DBGSTEP = 0;

}

void main(int argc, char* argv[]) {
	glutInit(&argc, argv);
   romstr = "BLK.ROM";
	
	FLAG_PSRAM = 1; FLAG_WINDOWED = 0;

	for(int i = 1; i < argc; i++) {
		if(argv[i][0] != '-') { romstr = argv[i]; continue; }
		if(!strcmp(argv[i], "-x")) FLAG_PSRAM = 0;
		if(!strcmp(argv[i], "-w")) FLAG_WINDOWED = 1;
		if(!strcmp(argv[i], "-h")) {
			printf("SS32 Emulator command line args: \n \
						XXXX.ROM rom name, BLK.ROM default \n \
						-x ignore PSRAM.bin file \n \
						-w windowed mode \n");
			exit(0);
		}
	}


	COLD();

   // OpenGL
   extra_init();

   // Threading
   /*
   for(int i = 1; i < 63; i++)
      pthread_create(&mthread[i], NULL, (void*)Mrun, (void*)i);
   //pthread_create(&sound_thread, NULL, Msound, NULL);
   //pthread_create(&dma_thread, NULL, MDMA, NULL);
   */

   Mrun(0);
   printf("GCLK: %d", GCLK);

}



