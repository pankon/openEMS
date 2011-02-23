/*
*	Copyright (C) 2011 Thorsten Liebig (Thorsten.Liebig@gmx.de)
*
*	This program is free software: you can redistribute it and/or modify
*	it under the terms of the GNU General Public License as published by
*	the Free Software Foundation, either version 3 of the License, or
*	(at your option) any later version.
*
*	This program is distributed in the hope that it will be useful,
*	but WITHOUT ANY WARRANTY{} without even the implied warranty of
*	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*	GNU General Public License for more details.
*
*	You should have received a copy of the GNU General Public License
*	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "openems_fdtd_mpi.h"
#include "FDTD/engine_interface_fdtd.h"
#include "FDTD/operator_mpi.h"
#include "FDTD/engine_mpi.h"
#include "Common/processfields.h"
#include "Common/processintegral.h"
#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <sstream>
#include <vector>
#include <iomanip>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "mpi.h"
#include "tools/useful.h"

openEMS_FDTD_MPI::openEMS_FDTD_MPI() : openEMS()
{
	m_MyID = MPI::COMM_WORLD.Get_rank();
	m_NumProc = MPI::COMM_WORLD.Get_size();

	m_MaxEnergy = 0;
	m_EnergyDecrement = 1;
	m_MPI_Op = NULL;

	if (m_NumProc>1)
		m_MPI_Enabled=true;
	else
		m_MPI_Enabled=false;

	if (m_MyID==0)
	{
		m_Gather_Buffer = new int[m_NumProc];
		m_Energy_Buffer = new double[m_NumProc];
	}
	else
	{
		m_Gather_Buffer = NULL;
		m_Energy_Buffer = NULL;
	}
}

openEMS_FDTD_MPI::~openEMS_FDTD_MPI()
{
	delete[] m_Gather_Buffer;
	delete[] m_Energy_Buffer;
}

bool openEMS_FDTD_MPI::parseCommandLineArgument( const char *argv )
{
	if (!argv)
		return false;

	bool ret = openEMS::parseCommandLineArgument( argv );

	if (ret)
		return ret;

	if (strcmp(argv,"--engine=MPI")==0)
	{
		cout << "openEMS_FDTD_MPI - enabled MPI parallel processing" << endl;
		m_engine = EngineType_MPI;
		return true;
	}

	return false;
}

bool openEMS_FDTD_MPI::SetupMPI(TiXmlElement* FDTD_Opts)
{
	//manipulate geometry for this part...
	UNUSED(FDTD_Opts);

	if (m_MPI_Enabled)
	{
		CSRectGrid* grid = m_CSX->GetGrid();
		int nz = grid->GetQtyLines(2);
		std::vector<unsigned int> jobs = AssignJobs2Threads(nz, m_NumProc);
		double z_lines[jobs.at(m_MyID)+1];

		if (m_MyID==0)
		{
			for (unsigned int n=0;n<jobs.at(0);++n)
				z_lines[n] = grid->GetLine(2,n);
			grid->ClearLines(2);
			grid->AddDiscLines(2,jobs.at(0),z_lines);

		}
		else
		{
			unsigned int z_start=0;
			for (int n=0;n<m_MyID;++n)
				z_start+=jobs.at(n);
			for (unsigned int n=0;n<=jobs.at(m_MyID);++n)
				z_lines[n] = grid->GetLine(2,z_start+n-1);
			grid->ClearLines(2);
			grid->AddDiscLines(2,jobs.at(m_MyID)+1,z_lines);
		}

		//lower neighbor is ID-1
		if (m_MyID>0)
			m_MPI_Op->SetNeighborDown(2,m_MyID-1);
		//upper neighbor is ID+1
		if (m_MyID<m_NumProc-1)
			m_MPI_Op->SetNeighborUp(2,m_MyID+1);

		m_MPI_Op->SetTag(0);
	}
	else
		cerr << "openEMS_FDTD_MPI::SetupMPI: Warning: Number of MPI processes is 1, skipping MPI engine... " << endl;

	return true;
}


bool openEMS_FDTD_MPI::SetupOperator(TiXmlElement* FDTD_Opts)
{
	bool ret = true;
	if (m_engine == EngineType_MPI)
	{
		FDTD_Op = Operator_MPI::New();
	}
	else
	{
		ret = openEMS::SetupOperator(FDTD_Opts);
	}

	m_MPI_Op = dynamic_cast<Operator_MPI*>(FDTD_Op);

	if ((m_MPI_Enabled) && (m_MPI_Op==NULL))
	{
		cerr << "openEMS_FDTD_MPI::SetupOperator: Error: MPI is enabled but requested engine does not support MPI... EXIT!!!" << endl;
		MPI_Barrier(MPI_COMM_WORLD);
		exit(0);
	}

	ret &=SetupMPI(FDTD_Opts);

	return ret;
}

unsigned int openEMS_FDTD_MPI::GetNextStep()
{
	//start processing and get local next step
	int step=PA->Process();
	double currTS = FDTD_Eng->GetNumberOfTimesteps();
	if ((step<0) || (step>(int)(NrTS - currTS))) step=NrTS - currTS;

	int local_step=step;

	//find the smallest next step requestes by all processings
	MPI_Reduce(&local_step, &step, 1, MPI_INT, MPI_MIN, 0, MPI_COMM_WORLD);
	//send the smallest next step to all
	MPI_Bcast(&step, 1, MPI_INT, 0, MPI_COMM_WORLD);

	return step;
}

bool openEMS_FDTD_MPI::CheckEnergyCalc()
{
	int local_Check = (int)m_ProcField->CheckTimestep();
	int result;

	//check if some process request an energy calculation --> the sum is larger than 0
	MPI_Reduce(&local_Check, &result, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
	//send result to all
	MPI_Bcast(&result, 1, MPI_INT, 0, MPI_COMM_WORLD);

	//calc energy if result is non-zero
	return result>0;
}

double openEMS_FDTD_MPI::CalcEnergy()
{
	double energy = 0;
	double loc_energy= m_ProcField->CalcTotalEnergy();

	//calc the sum of all local energies
	MPI_Reduce(&loc_energy, &energy, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
	//send sum-energy to all processes
	MPI_Bcast(&energy, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);

	if (energy>m_MaxEnergy)
		m_MaxEnergy = energy;
	if (m_MaxEnergy)
		m_EnergyDecrement = energy/m_MaxEnergy;

	return energy;
}

bool openEMS_FDTD_MPI::SetupProcessing()
{
	bool ret = openEMS::SetupProcessing();

	//search for active processings in different processes
	size_t numProc = PA->GetNumberOfProcessings();
	int active=0;
	bool deactivate = false;
	bool rename = false;
	for (size_t n=0;n<numProc;++n)
	{
		Processing* proc = PA->GetProcessing(n);
		int isActive = (int)proc->GetEnable();
		//sum of all active processings
		MPI_Reduce(&isActive, &active, 1, MPI_INT, MPI_SUM, 0, MPI_COMM_WORLD);
		deactivate = false;
		rename = false;
		if ((m_MyID==0) && (active>1)) //more than one active processing...
		{
			deactivate = true; //default
			if (dynamic_cast<ProcessIntegral*>(proc)!=NULL)
			{
				//type is integral processing --> disable! Needs to be fixed!
				cerr << "openEMS_FDTD_MPI::SetupProcessing(): Warning: Processing: " << proc->GetName() << " occures multiple times and is being deactivated..." << endl;
				deactivate = true;
				rename = false;
			}
			if (dynamic_cast<ProcessFields*>(proc)!=NULL)
			{
				//type is field processing --> renameing! Needs to be fixed!
				cerr << "openEMS_FDTD_MPI::SetupProcessing(): Warning: Processing: " << proc->GetName() << " occures multiple times and is being renamed..." << endl;
				deactivate = false;
				rename = true;
			}
		}
		//broadcast informations to all
		MPI_Bcast(&deactivate, 1, MPI::BOOL, 0, MPI_COMM_WORLD);
		MPI_Bcast(&rename, 1, MPI::BOOL, 0, MPI_COMM_WORLD);
		if (deactivate)
			proc->SetEnable(false);
		if (rename)
		{
			ProcessFields* ProcField = dynamic_cast<ProcessFields*>(proc);
			if (ProcField)
			{
				stringstream name_ss;
				name_ss << "ID" << m_MyID << "_" << ProcField->GetName();
				ProcField->SetName(name_ss.str());
				ProcField->SetFilePattern(name_ss.str());
				ProcField->SetFileName(name_ss.str());
			}
		}
	}
	return ret;
}

void openEMS_FDTD_MPI::RunFDTD()
{
	if (!m_MPI_Enabled)
		return openEMS::RunFDTD();

	cout << "Running MPI-FDTD engine... this may take a while... grab a cup of coffee?!?" << endl;

	//get the sum of all cells
	unsigned int local_NrCells=FDTD_Op->GetNumberCells();
	MPI_Reduce(&local_NrCells, &m_NumberCells, 1, MPI_UNSIGNED, MPI_SUM, 0, MPI_COMM_WORLD);
	MPI_Bcast(&m_NumberCells, 1, MPI_UNSIGNED, 0, MPI_COMM_WORLD);

	//special handling of a field processing, needed to realize the end criteria...
	m_ProcField = new ProcessFields(new Engine_Interface_FDTD(FDTD_Op,FDTD_Eng));
	PA->AddProcessing(m_ProcField);

	//init processings
	PA->InitAll();

	double currE=0;

	//add all timesteps to end-crit field processing with max excite amplitude
	unsigned int maxExcite = FDTD_Op->Exc->GetMaxExcitationTimestep();
	for (unsigned int n=0; n<FDTD_Op->Exc->Volt_Count; ++n)
		m_ProcField->AddStep(FDTD_Op->Exc->Volt_delay[n]+maxExcite);

	int prevTS=0,currTS=0;
	double speed = m_NumberCells/1e6;
	double t_diff;

	timeval currTime;
	gettimeofday(&currTime,NULL);
	timeval startTime = currTime;
	timeval prevTime= currTime;

	//*************** simulate ************//

	PA->PreProcess();
	int step = GetNextStep();

	while ((step>0) && !CheckAbortCond())
	{
		FDTD_Eng->IterateTS(step);
		step = GetNextStep();

		currTS = FDTD_Eng->GetNumberOfTimesteps();

		currE = 0;
		gettimeofday(&currTime,NULL);
		t_diff = CalcDiffTime(currTime,prevTime);

		if (CheckEnergyCalc())
			currE = CalcEnergy();

		//make sure all processes are at the same simulation time
		MPI_Bcast(&t_diff, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);

		if (t_diff>4)
		{
			if (currE==0)
				currE = CalcEnergy();
			if (m_MyID==0)
			{
				cout << "[@" << FormatTime(CalcDiffTime(currTime,startTime))  <<  "] Timestep: " << setw(12)  << currTS << " (" << setw(6) << setprecision(2) << std::fixed << (double)currTS/(double)NrTS*100.0  << "%)" ;
				cout << " || Speed: " << setw(6) << setprecision(1) << std::fixed << speed*(currTS-prevTS)/t_diff << " MC/s (" <<  setw(4) << setprecision(3) << std::scientific << t_diff/(currTS-prevTS) << " s/TS)" ;
				cout << " || Energy: ~" << setw(6) << setprecision(2) << std::scientific << currE << " (-" << setw(5)  << setprecision(2) << std::fixed << fabs(10.0*log10(m_EnergyDecrement)) << "dB)" << endl;

				//set step to zero to abort simulation and send to all
				if (m_EnergyDecrement<endCrit)
					step=0;
			}

			MPI_Bcast(&step, 1, MPI_INT, 0, MPI_COMM_WORLD);
			
			prevTime=currTime;
			prevTS=currTS;

			PA->FlushNext();
		}
	}
	PA->PostProcess();

	//*************** postproc ************//
	prevTime = currTime;
	gettimeofday(&currTime,NULL);

	t_diff = CalcDiffTime(currTime,startTime);

	if (m_MyID==0)
	{
		cout << "Time for " << FDTD_Eng->GetNumberOfTimesteps() << " iterations with " << FDTD_Op->GetNumberCells() << " cells : " << t_diff << " sec" << endl;
		cout << "Speed: " << speed*(double)FDTD_Eng->GetNumberOfTimesteps()/t_diff << " MCells/s " << endl;
	}
}