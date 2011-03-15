/* ----------------------------------------------------------------------
Copyright (2010) Aram Davtyan and Garegin Papoian

Papoian's Group, University of Maryland at Collage Park
http://papoian.chem.umd.edu/

Last Update: 12/01/2010
------------------------------------------------------------------------- */

#include "math.h"
#include "string.h"
#include "stdlib.h"
#include "fix_go-model.h"
#include "atom.h"
#include "timer.h"
#include "update.h"
#include "respa.h"
#include "comm.h"
#include "error.h"
#include "group.h"
#include "domain.h"
#include "fstream.h"
#include "random_park.h"

#include <iostream.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

using namespace LAMMPS_NS;

/* ---------------------------------------------------------------------- */

// {"ALA", "ARG", "ASN", "ASP", "CYS", "GLN", "GLU", "GLY", "HIS", "ILE", "LEU", "LYS", "MET", "PHE", "PRO", "SER", "THR", "TRP", "TYR", "VAL"};
// {"A", "R", "N", "D", "C", "Q", "E", "G", "H", "I", "L", "K", "M", "F", "P", "S", "T", "W", "Y", "V"};
//int se_map[] = {0, 0, 4, 3, 6, 13, 7, 8, 9, 0, 11, 10, 12, 2, 0, 14, 5, 1, 15, 16, 0, 19, 17, 0, 18, 0};

inline void FixGoModel::print_log(char *line)
{
  if (screen) fprintf(screen, line);
  if (logfile) fprintf(logfile, line);
}

FixGoModel::FixGoModel(LAMMPS *lmp, int narg, char **arg) :
  Fix(lmp, narg, arg)
{
	if (narg != 4) error->all("Illegal fix go-model command");
	
	restart_global = 1;

	char force_file_name[] = "forcesGO.dat";
	fout = fopen(force_file_name,"w");
	
	scalar_flag = 1;
	vector_flag = 1;
	size_vector = 3;
//	scalar_vector_freq = 1;
	extscalar = 1;
	extvector = 1;

	force_flag = 0;
	n = (int)(group->count(igroup)+1e-12);
	foriginal[0] = foriginal[1] = foriginal[2] = foriginal[3] = 0.0;
	
	seed = time(NULL);

	allocated = false;
	allocate();
	
//	fprintf(fout,"seed=%d rand->state=%d\n",seed, random->state());

	bonds_flag = angles_flag = dihedrals_flag = contacts_flag = contacts_dev_flag = 0;
	epsilon = epsilon2 = 1.0;

	int i, j;
	char varsection[30];
	ifstream in(arg[3]);
	if (!in) error->all("Coefficient file was not found!");
	while (!in.eof()) {
		in >> varsection;
		if (strcmp(varsection, "[Epsilon]")==0) {
			in >> epsilon;
		} else if (strcmp(varsection, "[Epsilon2]")==0) {
			in >> epsilon2;
		} else if (strcmp(varsection, "[Bonds]")==0) {
			bonds_flag = 1;
			in >> k_bonds;
			for (i=0;i<n-1;++i) in >> r0[i];
		} else if (strcmp(varsection, "[Angles]")==0) {
			angles_flag = 1;
			in >> k_angles;
			for (i=0;i<n-2;++i) in >> theta0[i];
		} else if (strcmp(varsection, "[Dihedrals]")==0) {
			dihedrals_flag = 1;
			in >> k_dihedrals[0] >> k_dihedrals[1];
			for (i=0;i<n-3;++i) in >> phi0[i];
		} else if (strcmp(varsection, "[Contacts]")==0) {
			contacts_flag = 1;
			for (i=0;i<n-4;++i) for (j=i;j<n-4;++j) in >> isNative[i][j];
			for (i=0;i<n-4;++i) for (j=i;j<n-4;++j) in >> sigma[i][j];
		} else if (strcmp(varsection, "[Contacts_Deviation]")==0) {
			contacts_dev_flag = 1;
			in >> sdivf; // Standart deviation in epsilon fractions
			in >> tcorr; // Correlation time in femtoseconds
			in >> dev0;  // Deviation on t=0
		}
	}
	in.close();

	if (contacts_dev_flag) {
		xi = 1/tcorr;
		w = sqrt(xi)*epsilon*sdivf;
		dev = dev0;

		devA = w*sqrt(2*update->dt);
		devB = xi*update->dt;
		devC = (1 - xi*update->dt/2);
	}
	
/*	fprintf(fout, "In constractor\n");
	fprintf(fout, "seed=%d\n", seed);
	fprintf(fout, "random->state=%d\n",random->state());
	fprintf(fout, "sdivf=%f\n", sdivf);
	fprintf(fout, "tcorr=%f\n", tcorr);
	fprintf(fout, "dev0=%f\n", dev0);
	fprintf(fout, "w=%f\n", w);
	fprintf(fout, "xi=%f\n", xi);
	fprintf(fout, "dt=%f\n", update->dt);
	fprintf(fout, "rand=%f\n", rand);
	fprintf(fout, "devA=%f\n", devA);
	fprintf(fout, "devB=%f\n", devB);
	fprintf(fout, "devC=%f\n", devC);
	fprintf(fout, "dev=%f\n\n", dev);*/

	x = atom->x;
	f = atom->f;
	image = atom->image;
	prd[0] = domain->xprd;
	prd[1] = domain->yprd;
	prd[2] = domain->zprd;
	half_prd[0] = prd[0]/2;
	half_prd[1] = prd[1]/2;
	half_prd[2] = prd[2]/2; 
	periodicity = domain->periodicity;

	Step = 0;
	sStep=0, eStep=0;
	ifstream in_rs("record_steps");
	in_rs >> sStep >> eStep;
	in_rs.close();
}

/* ---------------------------------------------------------------------- */

FixGoModel::~FixGoModel()
{
	if (allocated) {
		for (int i=0;i<n;i++) {
			delete [] xca[i];
		}

		for (int i=0;i<n-4;i++) {
			delete [] sigma[i];
			delete [] isNative[i];
		}

		delete [] r0;
		delete [] theta0;
		delete [] phi0;
		delete [] sigma;
		delete [] isNative;

		delete [] alpha_carbons;
		delete [] xca;
		delete [] res_no;
		delete [] res_info;

		delete random;
	}
}

/* ---------------------------------------------------------------------- */
inline int MIN(int a, int b)
{
	if ((a<b && a!=-1) || b==-1) return a;
	else return b;
}

inline int MAX(int a, int b)
{
	if ((a>b && a!=-1) || b==-1) return a;
	else return b;
}

inline bool FixGoModel::isFirst(int index)
{
	if (res_no[index]==1) return true;
	return false;
}

inline bool FixGoModel::isLast(int index)
{
	if (res_no[index]==n) return true;
	return false;
}

int FixGoModel::Tag(int index) {
	if (index==-1) return -1;
	return atom->tag[index];
}

inline void FixGoModel::Construct_Computational_Arrays()
{
	int *mask = atom->mask;
	int nlocal = atom->nlocal;
	int nall = atom->nlocal + atom->nghost;
	int *mol_tag = atom->molecule;

	int i, j, js;

	// Creating index arrays for Alpha_Carbons
	nn = 0;
	int last = 0;
	for (i = 0; i < n; ++i) {
		int min = -1, jm = -1;
		for (int j = 0; j < nall; ++j) {
			if (i==0 && mol_tag[j]<=0)
				error->all("Molecular tag must be positive in fix go-model");
			
			if ( (mask[j] & groupbit) && mol_tag[j]>last ) {
				if (mol_tag[j]<min || min==-1) {
					min = mol_tag[j];
					jm = j;
				}
			}
		}
		
		if (min==-1) break;

		alpha_carbons[nn] = jm;
		res_no[nn] = min;
		last = min;
		nn++;	
	}

	/*if (Step>=sStep && Step<=eStep) {
		fprintf(fout, "\n\n");
		for (i = 0; i < nn; ++i) {
			fprintf(fout, "%d ", res_no[i]);
		}
		fprintf(fout, "\n\n");
	}*/

	int nMinNeighbours = 3;
	int iLastLocal = -1;
	int lastResNo = -1;
	int lastResType = NONE;
	int nlastType = 0;

	// Checking sequance and marking residues
	for (i = 0; i < nn; ++i) {
		if (lastResNo!=-1 && lastResNo!=res_no[i]-1) {
			if (lastResType==LOCAL && res_no[i]!=n)
				error->all("Missing neighbor atoms in fix go-model (code: 001)");
			if (lastResType==GHOST) {
				if (iLastLocal!=-1 && i-nMinNeighbours<=iLastLocal)
					error->all("Missing neighbor atoms in fix go-model (code: 002)");
//				else {
//					js = i - nlastType;
//					if (iLastLocal!=-1) js = MAX(js, iLastLocal + nMinNeighbours + 1);
//					for (j=js;j<i;++j) res_info[j] = OFF;
//				}
			}
			
			iLastLocal = -1;
			lastResNo = -1;
			lastResType = NONE;
			nlastType = 0;
		}

		if (alpha_carbons[i]!=-1) {
			if (alpha_carbons[i]<nlocal) {
				if ( lastResType==OFF || (lastResType==GHOST && nlastType<nMinNeighbours && nlastType!=res_no[i]-1) ) {
					error->all("Missing neighbor atoms in fix go-model  (code: 003)");
				}
				iLastLocal = i;
				res_info[i] = LOCAL;
			} else {
//				if ( lastResType==GHOST && nlastType>=nMinNeighbours && (iLastLocal==-1 || i-2*nMinNeighbours-iLastLocal>=0) ) {
//					res_info[i-nMinNeighbours] = OFF;
//					nlastType = nMinNeighbours-1;
//				}

				res_info[i] = GHOST;
			}
		} else res_info[i] = OFF;

		if (lastResNo == res_no[i]) nlastType++; else nlastType = 0;

		lastResNo = res_no[i];
		lastResType = res_info[i];
	}
	if (lastResType==LOCAL && res_no[nn-1]!=n)
		error->all("Missing neighbor atoms in fix go-model  (code: 004)");
	if (lastResType==GHOST) {
		if (iLastLocal!=-1 && nn-nMinNeighbours<=iLastLocal)
			error->all("Missing neighbor atoms in fix go-model  (code: 005)");
//		else {
//			js = nn - nlastType;
//			if (iLastLocal!=-1) js = MAX(js, iLastLocal + nMinNeighbours + 1);
//			for (j=js;j<nn;++j) res_info[j] = OFF;
//		}
	}

/*	if (Step>=sStep && Step<=eStep) {
		for (i = 0; i < nn; ++i) {
			fprintf(fout, "%d ", res_info[i]);
		}
		fprintf(fout, "\n");
	}*/
}

void FixGoModel::allocate()
{
	alpha_carbons = new int[n];
	xca = new double*[n];
	res_no = new int[n];
	res_info = new int[n];

	r0 = new double[n-1];
	theta0 = new double[n-2];
	phi0 = new double[n-3];
	sigma = new double*[n-4];
	isNative = new bool*[n-4];

	for (int i = 0; i < n; ++i) {
		xca[i] = new double [3];
	}

	for (int i = 0; i < n-4; ++i) {
		sigma[i] = new double[n-4];
		isNative[i] = new bool[n-4];
	}

	random = new RanPark(lmp,seed);
	
	allocated = true;
}

/* ---------------------------------------------------------------------- */

int FixGoModel::setmask()
{
	int mask = 0;
	mask |= POST_FORCE;
	mask |= THERMO_ENERGY;
	mask |= POST_FORCE_RESPA;
	mask |= MIN_POST_FORCE;
	return mask;
}

/* ---------------------------------------------------------------------- */

void FixGoModel::init()
{
	if (strcmp(update->integrate_style,"respa") == 0)
		nlevels_respa = ((Respa *) update->integrate)->nlevels;
}

/* ---------------------------------------------------------------------- */

void FixGoModel::setup(int vflag)
{
	if (strcmp(update->integrate_style,"verlet") == 0)
		post_force(vflag);
	else {
		((Respa *) update->integrate)->copy_flevel_f(nlevels_respa-1);
		post_force_respa(vflag,nlevels_respa-1,0);
		((Respa *) update->integrate)->copy_f_flevel(nlevels_respa-1);
	}
}

/* ---------------------------------------------------------------------- */

void FixGoModel::min_setup(int vflag)
{
	post_force(vflag);
}

/* ---------------------------------------------------------------------- */

inline double adotb(double *a, double *b)
{
	return (a[0]*b[0] + a[1]*b[1] + a[2]*b[2]);
}

inline double cross(double *a, double *b, int index)
{
	switch (index) {
		case 0:
			return a[1]*b[2] - a[2]*b[1];
		case 1:
			return a[2]*b[0] - a[0]*b[2];
		case 2:
			return a[0]*b[1] - a[1]*b[0];
	}
	
	return 0;
}

inline double atan2(double y, double x)
{
	if (x==0) {
		if (y>0) return M_PI_2;
		else if (y<0) return -M_PI_2;
		else return NULL;
	} else {
		return atan(y/x) + (x>0 ? 0 : (y>=0 ? M_PI : -M_PI) );
	}
}

inline double FixGoModel::PeriodicityCorrection(double d, int i)
{
	if (!periodicity[i]) return d;
	else return ( d > half_prd[i] ? d - prd[i] : (d < -half_prd[i] ? d + prd[i] : d) );
}

void FixGoModel::compute_bond(int i) 
{
	int i_resno = res_no[i]-1;

	if (i_resno>=n-1) error->all("Wrong use of compute_bond() in fix go-model");

	double dx[3], r, dr, force;

	dx[0] = xca[i+1][0] - xca[i][0];
	dx[1] = xca[i+1][1] - xca[i][1];
	dx[2] = xca[i+1][2] - xca[i][2];

	r = sqrt(dx[0]*dx[0]+dx[1]*dx[1]+dx[2]*dx[2]);
	dr = r - r0[i_resno];
	force = 2*epsilon*k_bonds*dr/r;
	
	foriginal[0] += epsilon*k_bonds*dr*dr;

	f[alpha_carbons[i+1]][0] -= dx[0]*force;
	f[alpha_carbons[i+1]][1] -= dx[1]*force;
	f[alpha_carbons[i+1]][2] -= dx[2]*force;

	f[alpha_carbons[i]][0] -= -dx[0]*force;
	f[alpha_carbons[i]][1] -= -dx[1]*force;
	f[alpha_carbons[i]][2] -= -dx[2]*force;
}

void FixGoModel::compute_angle(int i) 
{
	int i_resno = res_no[i]-1;

	if (i_resno>=n-2) error->all("Wrong use of compute_angle() in fix go-model");

	double a[3], b[3], a2, b2, A, B, AB, adb, alpha;
	double theta, dtheta, force, factor[2][3];

	a[0] = xca[i+1][0] - xca[i][0];
	a[1] = xca[i+1][1] - xca[i][1];
	a[2] = xca[i+1][2] - xca[i][2];

	b[0] = xca[i+2][0] - xca[i+1][0];
	b[1] = xca[i+2][1] - xca[i+1][1];
	b[2] = xca[i+2][2] - xca[i+1][2];

	a2 = adotb(a, a);
	b2 = adotb(b, b);
	adb = adotb(a, b);
	A = sqrt(a2);
	B = sqrt(b2);
	AB = A*B;
	alpha = adb/AB;

	theta = acos(alpha);
	dtheta = theta - theta0[i_resno];
	dtheta = 180*dtheta/M_PI;
	force = 2*epsilon*k_angles*dtheta/(AB*sqrt(1-alpha*alpha));

	foriginal[0] += epsilon*k_angles*dtheta*dtheta;

	factor[0][0] = (b[0] - a[0]*adb/a2);
	factor[0][1] = (b[1] - a[1]*adb/a2);
	factor[0][2] = (b[2] - a[2]*adb/a2);

	factor[1][0] = (a[0] - b[0]*adb/b2);
	factor[1][1] = (a[1] - b[1]*adb/b2);
	factor[1][2] = (a[2] - b[2]*adb/b2);

	f[alpha_carbons[i]][0] -= factor[0][0]*force;
	f[alpha_carbons[i]][1] -= factor[0][1]*force;
	f[alpha_carbons[i]][2] -= factor[0][2]*force;

	f[alpha_carbons[i+1]][0] -= (factor[1][0] - factor[0][0])*force;
	f[alpha_carbons[i+1]][1] -= (factor[1][1] - factor[0][1])*force;
	f[alpha_carbons[i+1]][2] -= (factor[1][2] - factor[0][2])*force;

	f[alpha_carbons[i+2]][0] -= -factor[1][0]*force;
	f[alpha_carbons[i+2]][1] -= -factor[1][1]*force;
	f[alpha_carbons[i+2]][2] -= -factor[1][2]*force;
}

void FixGoModel::compute_dihedral(int i)
{
	int i_resno = res_no[i]-1;

	if (i_resno>=n-3) error->all("Wrong use of compute_dihedral() in fix go-model");

	double phi, dphi, y_slope[4][3], x_slope[4][3], force, V;
	double a[3], b[3], c[3];
	double bxa[3], cxa[3], cxb[3];
	double adb, bdc, adc, b2, bm, cdbxa;
	double X, Y, X2Y2;
	double dAngle_y, dAngle_x;
	double h1, h2, h3;
	int j, l;

	a[0] = xca[i+1][0] - xca[i][0];
	a[1] = xca[i+1][1] - xca[i][1];
	a[2] = xca[i+1][2] - xca[i][2];
	
	b[0] = xca[i+2][0] - xca[i+1][0];
	b[1] = xca[i+2][1] - xca[i+1][1];
	b[2] = xca[i+2][2] - xca[i+1][2];

	c[0] = xca[i+3][0] - xca[i+2][0];
	c[1] = xca[i+3][1] - xca[i+2][1];
	c[2] = xca[i+3][2] - xca[i+2][2];

	adb = adotb(a, b);
	bdc = adotb(b, c);
	adc = adotb(a, c);
	b2 = adotb(b, b);
	bm = sqrt(b2);
	
	bxa[0] = cross(b, a, 0);
	bxa[1] = cross(b, a, 1);
	bxa[2] = cross(b, a, 2);
	
	cxa[0] = cross(c, a, 0);
	cxa[1] = cross(c, a, 1);
	cxa[2] = cross(c, a, 2);
	
	cxb[0] = cross(c, b, 0);
	cxb[1] = cross(c, b, 1);
	cxb[2] = cross(c, b, 2);
	
	cdbxa = adotb(c, bxa);
	
	Y = -bm*cdbxa;
	X = adb*bdc - b2*adc;
	
	phi = atan2(Y, X);

	X2Y2 = (X*X + Y*Y);
	dAngle_y = X/X2Y2;
	dAngle_x = -Y/X2Y2;

	for (l=0;l<3;++l) {
		y_slope[0][l] = dAngle_y*bm*cxb[l];
		y_slope[1][l] = dAngle_y*( b[l]*cdbxa/bm - bm*(cxb[l] + cxa[l]) );
		y_slope[2][l] = dAngle_y*( -b[l]*cdbxa/bm + bm*(bxa[l] + cxa[l]) );
		y_slope[3][l] = -dAngle_y*bm*bxa[l];
	
		h1 = b[l]*bdc - c[l]*b2;
		h2 = a[l]*bdc - 2*b[l]*adc + c[l]*adb;
		h3 = b[l]*adb - a[l]*b2;
		x_slope[0][l] = -dAngle_x*h1;
		x_slope[1][l] = dAngle_x*(h1 - h2);
		x_slope[2][l] = dAngle_x*(h2 - h3);
		x_slope[3][l] = dAngle_x*h3;
	}
	
	for (j=0;j<2;j++) { 
		dphi = (2*j+1)*(phi - phi0[i_resno]);
		V = epsilon*k_dihedrals[j]*(1-cos(dphi));

		force = (2*j+1)*epsilon*k_dihedrals[j]*sin(dphi);

		foriginal[0] += V;

		for (l=0; l<3; l++) {
			f[alpha_carbons[i]][l] -= force*(y_slope[0][l] + x_slope[0][l]);
			f[alpha_carbons[i+1]][l] -= force*(y_slope[1][l] + x_slope[1][l]);
			f[alpha_carbons[i+2]][l] -= force*(y_slope[2][l] + x_slope[2][l]);
			f[alpha_carbons[i+3]][l] -= force*(y_slope[3][l] + x_slope[3][l]);
		}
	}
	
}

/*  Correlated noise generator for contact potential
    dev(t+dt) = dev(t) - 0.5*xi*(dev(t) - devp(t))*dt + w*sqrt(2*dt)*rand
    devp(t) = dev(t) - xi*dev(t)*dt + w*sqrt(2*dt)*rand

    The formula above was simplified to
    dev += (w*sqrt(2*dt)*rand - xi*dt*dev)*(1 - xi*dt/2)

    devA = w*sqrt(2*dt)
    devB = xi*dt
    devC = (1 - xi*dt/2) 

    <dev(t)dev(t+dt)> = w^2/xi * Exp[-xi*dt]
*/
void FixGoModel::compute_contact_deviation()
{
//	fprintf(fout,"compute_contact_deviation on step %d\n",Step);
	rand = random->gaussian();
	dev += (devA*rand - devB*dev)*devC;
}

void FixGoModel::compute_contact(int i, int j)
{
	int i_resno = res_no[i]-1;
	int j_resno = res_no[j]-1;

	if (i_resno>=n-4 || j_resno<=i_resno+3) error->all("Wrong use of compute_contact() in fix go-model");

	double dx[3], r, rsq, sgrinv, sgrinv12, sgrinv10;
	double V, force, contact_epsilon;

	dx[0] = xca[j][0] - xca[i][0];
	dx[1] = xca[j][1] - xca[i][1];
	dx[2] = xca[j][2] - xca[i][2];

	rsq = dx[0]*dx[0]+dx[1]*dx[1]+dx[2]*dx[2];
	r = sqrt(rsq);

	if (r>sigma[i_resno][j_resno-4]*3) return;

	sgrinv = sigma[i_resno][j_resno-4]/r;
	sgrinv12 = pow(sgrinv, 12);
	sgrinv10 = pow(sgrinv, 10);

	if (isNative[i_resno][j_resno-4]) {
		contact_epsilon = epsilon;
		if (contacts_dev_flag) contact_epsilon = epsilon + dev;

		V = contact_epsilon*(5*sgrinv12 - 6*sgrinv10);
		force = -60*contact_epsilon*(sgrinv12 - sgrinv10)/rsq;
	} else {
		V = epsilon2*sgrinv12;
		force = -12*epsilon2*sgrinv12/rsq;
	}

	foriginal[0] += V;

	f[alpha_carbons[j]][0] -= force*dx[0];
	f[alpha_carbons[j]][1] -= force*dx[1];
	f[alpha_carbons[j]][2] -= force*dx[2];

	f[alpha_carbons[i]][0] -= -force*dx[0];
	f[alpha_carbons[i]][1] -= -force*dx[1];
	f[alpha_carbons[i]][2] -= -force*dx[2];
}

void FixGoModel::out_xyz_and_force(int coord)
{
//	out.precision(12);
	
	fprintf(fout, "%d\n", Step);
	fprintf(fout, "%d%d%d%d\n", bonds_flag, angles_flag, dihedrals_flag, contacts_flag);
	fprintf(fout, "Number of atoms %d\n", n);
	fprintf(fout, "Energy: %d\n\n", foriginal[0]);

	int index;	

	if (coord==1) {
		fprintf(fout, "rca = {");
		for (int i=0;i<nn;i++) {
			index = alpha_carbons[i];
			if (index!=-1) {
				fprintf(fout, "{%.8f, %.8f, %.8f}", x[index][0], x[index][1], x[index][2]);
				if (i!=nn-1) fprintf(fout, ",\n");
			}
		}
		fprintf(fout, "};\n\n\n");
	}
	

	fprintf(fout, "fca = {");
	for (int i=0;i<nn;i++) {
		index = alpha_carbons[i];
		if (index!=-1) {
			fprintf(fout, "{%.8f, %.8f, %.8f}", f[index][0], f[index][1], f[index][2]);
			if (i!=nn-1) fprintf(fout, ",\n");

		}
	}
	fprintf(fout, "};\n\n\n\n");
}

void FixGoModel::compute_goModel()
{
	ntimestep = update->ntimestep;

	Step++;

	if(atom->nlocal==0) return;

	Construct_Computational_Arrays();

	x = atom->x;
        f = atom->f;
        image = atom->image;

	int i, j, xbox, ybox, zbox;
	int i_resno, j_resno;
	
	foriginal[0] = foriginal[1] = foriginal[2] = foriginal[3] = 0.0;
	force_flag = 0;

	for (i=0;i<nn;++i) {
		if (res_info[i]==LOCAL) {
			foriginal[1] += f[alpha_carbons[i]][0];
			foriginal[2] += f[alpha_carbons[i]][1];
			foriginal[3] += f[alpha_carbons[i]][2];
		}

		// Calculating xca Ca atoms coordinates array
		if ( (res_info[i]==LOCAL || res_info[i]==GHOST) ) {
			if (domain->xperiodic) {
				xbox = (image[alpha_carbons[i]] & 1023) - 512;
				xca[i][0] = x[alpha_carbons[i]][0] + xbox*prd[0];
			} else xca[i][0] = x[alpha_carbons[i]][0];
			if (domain->yperiodic) {
				ybox = (image[alpha_carbons[i]] >> 10 & 1023) - 512;
				xca[i][1] = x[alpha_carbons[i]][1] + ybox*prd[1];
			} else xca[i][1] = x[alpha_carbons[i]][1];
			if (domain->zperiodic) {
				zbox = (image[alpha_carbons[i]] >> 20) - 512;
				xca[i][2] = x[alpha_carbons[i]][2] + zbox*prd[2];
			} else xca[i][2] = x[alpha_carbons[i]][2];
		}
	}

/*	for (i=0;i<nn;++i) {
		if (res_info[i]!=LOCAL) continue;

		if (bonds_flag && res_no[i]<=n-1)
			compute_bond(i);

		if (angles_flag && res_no[i]<=n-2)
                        compute_angle(i);

		if (dihedrals_flag && res_no[i]<=n-3)
                        compute_dihedral(i);

		for (j=i+1;j<nn;++j) {
			if (res_info[j]!=LOCAL && res_info[j]!=GHOST) continue;

                        if (contacts_flag && res_no[i]<res_no[j]-3)
                                compute_contact(i, j);
                }
	}*/

	double tmp, tmp2;
	double tmp_time;
	int me,nprocs;
  	MPI_Comm_rank(world,&me);
  	MPI_Comm_size(world,&nprocs);
	if (Step>=sStep && Step<=eStep) {
		fprintf(fout, "At Start %d:\n", nn);
		out_xyz_and_force(1);
	}
	
/*	if (Step>=sStep && Step<=eStep) {
		fprintf(fout, "Before compute_contact_deviation\n");
		fprintf(fout, "seed=%d\n", seed);
		fprintf(fout, "random->state=%d\n",random->state());
		fprintf(fout, "sdivf=%f\n", sdivf);
		fprintf(fout, "tcorr=%f\n", tcorr);
		fprintf(fout, "dev0=%f\n", dev0);
		fprintf(fout, "w=%f\n", w);
		fprintf(fout, "xi=%f\n", xi);
		fprintf(fout, "dt=%f\n", update->dt);
		fprintf(fout, "rand=%f\n", rand);
		fprintf(fout, "devA=%f\n", devA);
		fprintf(fout, "devB=%f\n", devB);
		fprintf(fout, "devC=%f\n", devC);
		fprintf(fout, "dev=%f\n\n", dev);
	}*/

	if (contacts_dev_flag)
		compute_contact_deviation();

/*	if (Step>=sStep && Step<=eStep) {
		fprintf(fout, "After compute_contact_deviation\n");
		fprintf(fout, "seed=%d\n", seed);
		fprintf(fout, "random->state=%d\n",random->state());
		fprintf(fout, "sdivf=%f\n", sdivf);
		fprintf(fout, "tcorr=%f\n", tcorr);
		fprintf(fout, "dev0=%f\n", dev0);
		fprintf(fout, "w=%f\n", w);
		fprintf(fout, "xi=%f\n", xi);
		fprintf(fout, "dt=%f\n", update->dt);
		fprintf(fout, "rand=%f\n", rand);
		fprintf(fout, "devA=%f\n", devA);
		fprintf(fout, "devB=%f\n", devB);
		fprintf(fout, "devC=%f\n", devC);
		fprintf(fout, "dev=%f\n\n", dev);
	}*/

	tmp = foriginal[0];
	for (i=0;i<nn;i++) {
		if (bonds_flag && res_info[i]==LOCAL && res_no[i]<=n-1)
			compute_bond(i);
	}

	if (bonds_flag && Step>=sStep && Step<=eStep) {
		fprintf(fout, "Bonds %d:\n", nn);
		fprintf(fout, "Bonds_Energy: %.12f\n", foriginal[0]-tmp);
		out_xyz_and_force();
	}

	tmp = foriginal[0];
//	fprintf(fout, "\n{");
	for (i=0;i<nn;i++) {
//		tmp2=foriginal[0];
		if (angles_flag && res_info[i]==LOCAL && res_no[i]<=n-2)
			compute_angle(i);
//		if (Step>=sStep && Step<=eStep) {
//			fprintf(fout, "%.12f, ", foriginal[0]-tmp2);
//		}
	}
//	fprintf(fout, "}\n\n");

	if (angles_flag && Step>=sStep && Step<=eStep) {
		fprintf(fout, "Angles %d:\n", nn);
		fprintf(fout, "Angles_Energy: %.12f\n", foriginal[0]-tmp);
		out_xyz_and_force();
	}

	tmp = foriginal[0];
//	if (Step>=sStep && Step<=eStep) fprintf(fout, "\n{");
	for (i=0;i<nn;i++) {
//		tmp2=foriginal[0];
		if (dihedrals_flag && res_info[i]==LOCAL && res_no[i]<=n-3)
			compute_dihedral(i);
//		if (Step>=sStep && Step<=eStep) {
//			fprintf(fout, "%.12f, ", foriginal[0]-tmp2);
//		}
	}
//	if (Step>=sStep && Step<=eStep) fprintf(fout, "}\n\n");

	if (dihedrals_flag && Step>=sStep && Step<=eStep) {
		fprintf(fout, "Dihedrals %d:\n", nn);
		fprintf(fout, "Dihedrals_Energy: %.12f\n", foriginal[0]-tmp);
		out_xyz_and_force();
	}

	tmp = foriginal[0];
//	if (Step>=sStep && Step<=eStep) fprintf(fout, "\n{");
	for (i=0;i<nn;i++) {
		for (j=0;j<nn;j++) {
			tmp2=foriginal[0];
			if (contacts_flag && res_info[i]==LOCAL && (res_info[j]==LOCAL || res_info[j]==GHOST) && res_no[i]<res_no[j]-3)
				compute_contact(i, j);
//			if (Step>=sStep && Step<=eStep) {
//				fprintf(fout, "%.12f, ", foriginal[0]-tmp2);
//			}
		}
	}
//	if (Step>=sStep && Step<=eStep) fprintf(fout, "}\n\n");

	if (contacts_flag && Step>=sStep && Step<=eStep) {
		fprintf(fout, "Contacts %d:\n", nn);
		fprintf(fout, "Contacts_Energy: %.12f\n", foriginal[0]-tmp);
		out_xyz_and_force();
	}

	if (Step>=sStep && Step<=eStep) {
		fprintf(fout, "All:\n");
		out_xyz_and_force(1);
		fprintf(fout, "\n\n\n");
	}
}

/* ---------------------------------------------------------------------- */

void FixGoModel::post_force(int vflag)
{
	compute_goModel();
}

/* ---------------------------------------------------------------------- */

void FixGoModel::post_force_respa(int vflag, int ilevel, int iloop)
{
	if (ilevel == nlevels_respa-1) post_force(vflag);
}

/* ---------------------------------------------------------------------- */

void FixGoModel::min_post_force(int vflag)
{
	post_force(vflag);
}

/* ----------------------------------------------------------------------
	 potential energy of added force
------------------------------------------------------------------------- */

double FixGoModel::compute_scalar()
{
	// only sum across procs one time

	if (force_flag == 0) {
		MPI_Allreduce(foriginal,foriginal_all,4,MPI_DOUBLE,MPI_SUM,world);
		force_flag = 1;
	}
	return foriginal_all[0];
}

/* ----------------------------------------------------------------------
	 return components of total force on fix group before force was changed
------------------------------------------------------------------------- */

double FixGoModel::compute_vector(int n)
{
	// only sum across procs one time

	if (force_flag == 0) {
		MPI_Allreduce(foriginal,foriginal_all,4,MPI_DOUBLE,MPI_SUM,world);
		force_flag = 1;
	}
	return foriginal_all[n+1];
}

/* ----------------------------------------------------------------------
   pack entire state of Fix into one write
------------------------------------------------------------------------- */

void FixGoModel::write_restart(FILE *fp)
{
/*	fprintf(fout, "In write_restart\n");
	fprintf(fout, "seed=%d\n", seed);
	fprintf(fout, "sdivf=%f\n", sdivf);
	fprintf(fout, "tcorr=%f\n", tcorr);
	fprintf(fout, "dev0=%f\n", dev0);
	fprintf(fout, "w=%f\n", w);
	fprintf(fout, "xi=%f\n", xi);
	fprintf(fout, "dt=%f\n", update->dt);
	fprintf(fout, "rand=%f\n", rand);
	fprintf(fout, "devA=%f\n", devA);
	fprintf(fout, "devB=%f\n", devB);
	fprintf(fout, "devC=%f\n", devC);
	fprintf(fout, "dev=%f\n", dev);
	fprintf(fout, "random->state=%d\n\n",random->state());*/ 
	
  int n = 0;
  double list[6];
  list[n++] = contacts_dev_flag;
  list[n++] = random->state();
  if (contacts_dev_flag) {
    list[n++] = sdivf;
    list[n++] = tcorr;
    list[n++] = dev0;
    list[n++] = dev;
  }

  if (comm->me == 0) {
    int size = n * sizeof(double);
    fwrite(&size,sizeof(int),1,fp);
    fwrite(&list,sizeof(double),n,fp);
  }
}

/* ----------------------------------------------------------------------
   use state info from restart file to restart the Fix
------------------------------------------------------------------------- */

void FixGoModel::restart(char *buf)
{	
  int n = 0;
  double *list = (double *) buf;

  contacts_dev_flag = static_cast<bool> (list[n++]);
  seed = static_cast<int> (list[n++]);
  if (contacts_dev_flag) {
	sdivf = list[n++];
	tcorr = list[n++];
	dev0 = list[n++];
	dev = list[n++];
	
	xi = 1/tcorr;
	w = sqrt(xi)*epsilon*sdivf;

	devA = w*sqrt(2*update->dt);
	devB = xi*update->dt;
	devC = (1 - xi*update->dt/2);
  }

  random->reset(seed);

/*	fprintf(fout, "In restart\n");
	fprintf(fout, "seed=%d\n", seed);
	fprintf(fout, "random->state=%d\n",random->state());
	fprintf(fout, "sdivf=%f\n", sdivf);
	fprintf(fout, "tcorr=%f\n", tcorr);
	fprintf(fout, "dev0=%f\n", dev0);
	fprintf(fout, "w=%f\n", w);
	fprintf(fout, "xi=%f\n", xi);
	fprintf(fout, "dt=%f\n", update->dt);
	fprintf(fout, "rand=%f\n", rand);
	fprintf(fout, "devA=%f\n", devA);
	fprintf(fout, "devB=%f\n", devB);
	fprintf(fout, "devC=%f\n", devC);
	fprintf(fout, "dev=%f\n\n", dev);*/
}