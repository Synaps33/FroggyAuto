#include "common.h"

#include "IniFile.h"

#include "CarCtrl.h"
#include "FileMgr.h"
#include "main.h"
#include "Population.h"

#if defined(RW_SOFT)
float CIniFile::PedNumberMultiplier = 0.4f;
float CIniFile::CarNumberMultiplier = 0.4f;
#else
float CIniFile::PedNumberMultiplier = 0.6f;
float CIniFile::CarNumberMultiplier = 0.6f;
#endif

void CIniFile::LoadIniFile()
{
	CFileMgr::SetDir("");
#if 0
	int f = CFileMgr::OpenFile("gta3.ini", "r");
	if (f){
		CFileMgr::ReadLine(f, gString, 200);
		sscanf(gString, "%f", &PedNumberMultiplier);
		PedNumberMultiplier = Min(3.0f, Max(0.5f, PedNumberMultiplier));
		CFileMgr::ReadLine(f, gString, 200);
		sscanf(gString, "%f", &CarNumberMultiplier);
		CarNumberMultiplier = Min(3.0f, Max(0.5f, CarNumberMultiplier));
		CFileMgr::CloseFile(f);
	}
#endif
	CPopulation::MaxNumberOfPedsInUse = DEFAULT_MAX_NUMBER_OF_PEDS * PedNumberMultiplier;
	CCarCtrl::MaxNumberOfCarsInUse = DEFAULT_MAX_NUMBER_OF_CARS * CarNumberMultiplier;
}
