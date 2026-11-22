#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL_Image.h>
#include "XPad.h"
#include <SDL.h>

int axisL, axisLY, axisR, axisRY;
int xpos=0, ypos=0, xstretch=0, ystretch=0; 

void saveScreen()
{
	FILE *fp;

	fp=fopen("U:\\screensize.xsv","w+");

	fprintf(fp, "XPOS=%d\n", xpos);
	fprintf(fp, "YPOS=%d\n", ypos);
	fprintf(fp, "XSTRETCH=%d\n", xstretch);
	fprintf(fp, "YSTRETCH=%d\n", ystretch);

	fclose(fp);

	return;

}
void loadScreen()
{
	char str[500];
	FILE * fi;
	fi = fopen("U:\\screensize.xsv", "r");

	if(fi != NULL) 
	{
		do
		{
			fgets(str, sizeof(str), fi);
			if (!feof(fi))
			{

				if (strstr(str, "XPOS=") == str){

					xpos = atoi(str + 5);

				}
				else if (strstr(str, "YPOS=") == str){

					ypos = atoi(str + 5);

				}
				else if (strstr(str, "XSTRETCH=") == str){

					xstretch = atoi(str + 9);

				}
				else if (strstr(str, "YSTRETCH=") == str){

					ystretch = atoi(str + 9);

				}
			}
		}
		while (!feof(fi));

		fclose(fi);
	}
	else
	{
		xpos = -215;
		ypos = 25;
		xstretch = 920;
		ystretch = 790;
	}

	//If user has changed screen size
	if((xstretch + ystretch + xpos + ypos) != 0)
	{
		//Set Screen Size 
		SDL_XBOX_SetScreenStretch(xstretch, ystretch);
		SDL_XBOX_SetScreenPosition(xpos, ypos);
	}
	return;
}

void resize_screen(SDL_Joystick *JOYPAD, SDL_Surface *screen_sfc)
{
	int done = 0, button;
	SDL_Rect src, dest;
	SDL_Event event;

	SDL_Surface * IMG_RESIZE;

	IMG_RESIZE = IMG_Load("D:\\other\\dont_delete_me.png");

	dest.x = 0;
	dest.y = 0;
	dest.w = 640;
	dest.h = 400;

	SDL_FillRect(screen_sfc, &dest,
		SDL_MapRGB(screen_sfc->format, 0x00, 0x00, 0x00));
	src.x = 0;
	src.y = 0;
	src.w = IMG_RESIZE->w;
	src.h = IMG_RESIZE->h;
	dest.x = 0;
	dest.y = 0;
	dest.w = IMG_RESIZE->w;
	dest.h = IMG_RESIZE->h;
	SDL_BlitSurface(IMG_RESIZE, NULL, screen_sfc, &dest);
	SDL_UpdateRect(screen_sfc, 0, 0, 640, 480);


	do{
		axisL = SDL_JoystickGetAxis(JOYPAD, JOY_AXIS_LX);
		axisLY = SDL_JoystickGetAxis(JOYPAD, JOY_AXIS_LY);
		axisR = SDL_JoystickGetAxis(JOYPAD, JOY_AXIS_RX);
		axisRY = SDL_JoystickGetAxis(JOYPAD, JOY_AXIS_RY);               

		// y = top to bottom
		// x = left to right

		if (axisL < -JOY_DEADZONE)
		{
			xstretch = xstretch - 4;
		}
		else if (axisL > JOY_DEADZONE)
		{
			xstretch = xstretch + 4;


		}

		else if (axisLY < -JOY_DEADZONE)
		{
			ystretch = ystretch - 3;

		}
		else if (axisLY > JOY_DEADZONE)
		{
			ystretch = ystretch + 3;
		}

		//Screen RePosition
		if (axisR < -JOY_DEADZONE)
		{
			xpos = xpos - 1;
		}

		else if (axisR > JOY_DEADZONE)
		{
			xpos = xpos + 1;
		}

		else if (axisRY < -JOY_DEADZONE)
		{
			ypos = ypos - 1;
		}
		else if (axisRY > JOY_DEADZONE)
		{
			ypos = ypos + 1;
		}

		while (SDL_PollEvent(&event)){

			if (event.type == SDL_JOYBUTTONDOWN)
			{
				button = event.jbutton.button;
				if (button == JOY_BTTN_A)
				{
					done = 1;

				}
			}
		}

		SDL_XBOX_SetScreenPosition(xpos, ypos);
		SDL_XBOX_SetScreenStretch(xstretch, ystretch);
	}
	while (done == 0);

	saveScreen();
	SDL_FreeSurface(IMG_RESIZE);

	return;
}