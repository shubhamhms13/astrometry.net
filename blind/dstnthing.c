
#include <stdio.h>

#include "bl.h"
#include "blind_wcs.h"
#include "sip.h"
#include "sip_qfits.h"
#include "log.h"
#include "errors.h"
#include "tweak.h"
#include "matchfile.h"
#include "matchobj.h"
#include "boilerplate.h"
#include "xylist.h"
#include "rdlist.h"
#include "mathutil.h"
#include "verify.h"

static const char* OPTIONS = "hx:m:r:vj:p:";

void print_help(char* progname) {
	boilerplate_help_header(stdout);
	printf("\nUsage: %s\n"
		   //"   -w <WCS input file>\n"
		   "   -m <match input file>\n"
		   "   -x <xyls input file>\n"
		   "   -r <rdls input file>\n"
		   "   [-p <plot output file>]\n"
           "   [-v]: verbose\n"
		   "   [-j <pixel-jitter>]: set pixel jitter (default 1.0)\n"
		   "\n", progname);
}

extern char *optarg;
extern int optind, opterr, optopt;
/*
 wget "http://antwrp.gsfc.nasa.gov/apod/image/0403/cmsky_cortner_full.jpg"
 #solve-field --backend-config backend.cfg -v --keep-xylist %s.xy --continue --scale-low 10 --scale-units degwidth cmsky_cortner_full.xy --no-tweak
 cp cmsky_cortner_full.xy 1.xy
 cp cmsky_cortner_full.rdls 1.rd
 cp cmsky_cortner_full.wcs 1.wcs
 cp cmsky_cortner_full.jpg 1.jpg
 wget "http://live.astrometry.net/status.php?job=alpha-201003-01883980&get=match.fits" -O 1.match
 */

int main(int argc, char** args) {
	int c;

	char* xylsfn = NULL;
	//char* wcsfn = NULL;
	char* matchfn = NULL;
	char* rdlsfn = NULL;
	char* plotfn = NULL;

	double pixeljitter = 1.0;
	int i;
	int W, H;
	xylist_t* xyls = NULL;
	rdlist_t* rdls = NULL;
	matchfile* mf;
	MatchObj* mo;
	sip_t sip;

	int loglvl = LOG_MSG;
	//double crpix[] = { HUGE_VAL, HUGE_VAL };
	//FILE* logstream = stderr;

	//fits_use_error_system();

    while ((c = getopt(argc, args, OPTIONS)) != -1) {
        switch (c) {
		case 'p':
			plotfn = optarg;
			break;
		case 'j':
			pixeljitter = atof(optarg);
			break;
        case 'h':
			print_help(args[0]);
			exit(0);
		case 'r':
			rdlsfn = optarg;
			break;
		case 'x':
			xylsfn = optarg;
			break;
			/*
			 case 'w':
			 wcsfn = optarg;
			 break;
			 */
		case 'm':
			matchfn = optarg;
			break;
        case 'v':
            loglvl++;
            break;
			/*
			 case 'X':
			 crpix[0] = atof(optarg);
			 break;
			 case 'Y':
			 crpix[1] = atof(optarg);
			 break;
			 */
		}
	}
	if (optind != argc) {
		print_help(args[0]);
		exit(-1);
	}
	if (!xylsfn || !matchfn || !rdlsfn) {
		print_help(args[0]);
		exit(-1);
	}

	//log_to(logstream);
	log_init(loglvl);
	//errors_log_to(logstream);

	/*
	 if (W == 0 || H == 0) {
	 logerr("Need -W, -H\n");
	 exit(-1);
	 }
	 if (crpix[0] == HUGE_VAL)
	 crpix[0] = W/2.0;
	 if (crpix[1] == HUGE_VAL)
	 crpix[1] = H/2.0;
	 */


	// read XYLS.
	xyls = xylist_open(xylsfn);
	if (!xyls) {
		logmsg("Failed to read an xylist from file %s.\n", xylsfn);
		exit(-1);
	}

	// read RDLS.
	rdls = rdlist_open(rdlsfn);
	if (!rdls) {
		logmsg("Failed to read an rdlist from file %s.\n", rdlsfn);
		exit(-1);
	}

	// image W, H
	W = xylist_get_imagew(xyls);
	H = xylist_get_imageh(xyls);
	if ((W == 0.0) || (H == 0.0)) {
		logmsg("XYLS file %s didn't contain IMAGEW and IMAGEH headers.\n", xylsfn);
		exit(-1);
	}

	// read match file.
	mf = matchfile_open(matchfn);
	if (!mf) {
		ERROR("Failed to read match file %s", matchfn);
		exit(-1);
	}
	mo = matchfile_read_match(mf);
	if (!mo) {
		ERROR("Failed to read match from file %s", matchfn);
		exit(-1);
	}

	sip_wrap_tan(&mo->wcstan, &sip);



	// (x,y) positions of field stars.
	double* fieldpix;
	int Nfield;
	double* indexpix;
	starxy_t* xy;
	rd_t* rd;
	int Nindex;

	xy = xylist_read_field(xyls, NULL);
	if (!xy) {
		logmsg("Failed to read xyls entries.\n");
		exit(-1);
	}
	Nfield = starxy_n(xy);
	fieldpix = starxy_to_xy_array(xy, NULL);
	logmsg("Found %i field objects\n", Nfield);

	// Project RDLS into pixel space.
	rd = rdlist_read_field(rdls, NULL);
	if (!rd) {
		logmsg("Failed to read rdls entries.\n");
		exit(-1);
	}
	Nindex = rd_n(rd);
	logmsg("Found %i index objects\n", Nindex);
	indexpix = malloc(2 * Nindex * sizeof(double));
	for (i=0; i<Nindex; i++) {
		bool ok;
		double ra = rd_getra(rd, i);
		double dec = rd_getdec(rd, i);
		ok = sip_radec2pixelxy(&sip, ra, dec, indexpix + i*2, indexpix + i*2 + 1);
		assert(ok);
	}
	logmsg("CRPIX is (%g,%g)\n", sip.wcstan.crpix[0], sip.wcstan.crpix[1]);

	double* fieldsigma2s = malloc(Nfield * sizeof(double));
	int besti;
	int* theta;
	double logodds;
	double Q2, R2;
	double qc[2];
	double gamma;

	// quad radius-squared = AB distance.
	Q2 = distsq(mo->quadpix, mo->quadpix + 2, 2);
	qc[0] = sip.wcstan.crpix[0];
	qc[1] = sip.wcstan.crpix[1];

	// HACK -- variance growth rate wrt radius.
	gamma = 1.0;

	for (i=0; i<Nfield; i++) {
		R2 = distsq(qc, fieldpix + 2*i, 2);
		fieldsigma2s[i] = square(pixeljitter) * (1.0 + gamma * R2/Q2);
	}

	logodds = verify_star_lists(indexpix, Nindex,
								fieldpix, fieldsigma2s, Nfield,
								W*H, 0.25,
								log(1e-100), log(1e100),
								&besti, NULL, &theta, NULL);
	logmsg("Logodds: %g\n", logodds);

	/*
	if (plotfn) {
				plot_args_t pargs;
				plotimage_t* img;
				cairo_t* cairo;

				plotstuff_init(&pargs);
				pargs.outformat = PLOTSTUFF_FORMAT_PNG;
				pargs.outfn = plotfn;
				img = plotstuff_get_config(&pargs, "image");
				img->format = PLOTSTUFF_FORMAT_JPG;
				plot_image_set_filename(img, "1.jpg");
				plot_image_setsize(&pargs, img);
				plotstuff_run_command(&pargs, "image");
				cairo = pargs.cairo;
				// red circles around every field star.
				cairo_set_color(cairo, "red");
				for (i=0; i<Nfield; i++) {
					cairoutils_draw_marker(cairo, CAIROUTIL_MARKER_CIRCLE,
										   fieldpix[2*i+0], fieldpix[2*i+1],
										   2.0 * sqrt(fieldsigma2s[i]));
					cairo_stroke(cairo);
				}
				// green crosshairs at every index star.
				cairo_set_color(cairo, "green");
				for (i=0; i<Nindex; i++) {
					cairoutils_draw_marker(cairo, CAIROUTIL_MARKER_XCROSSHAIR,
										   indexpix[2*i+0], indexpix[2*i+1],
										   3);
					cairo_stroke(cairo);
				}

				// thick white circles for corresponding field stars.
				cairo_set_line_width(cairo, 2);
				for (i=0; i<Nfield; i++) {
					if (theta[i] < 0)
						continue;
					cairo_set_color(cairo, "white");
					cairoutils_draw_marker(cairo, CAIROUTIL_MARKER_CIRCLE,
										   fieldpix[2*i+0], fieldpix[2*i+1],
										   2.0 * sqrt(fieldsigma2s[i]));
					cairo_stroke(cairo);
					// thick cyan crosshairs for corresponding index stars.
					cairo_set_color(cairo, "cyan");
					cairoutils_draw_marker(cairo, CAIROUTIL_MARKER_XCROSSHAIR,
										   indexpix[2*theta[i]+0],
										   indexpix[2*theta[i]+1],
										   3);
					cairo_stroke(cairo);
					
				}

				plotstuff_output(&pargs);
			}

			free(theta);
			free(fieldsigma2s);
		}

		free(fieldpix);
		free(indexpix);
	}



	if (xylist_close(xyls)) {
		logmsg("Failed to close XYLS file.\n");
	}
	return 0;














	while (1) {
		double x,y,ra,dec;
		if (fscanf(stdin, "%lf %lf %lf %lf\n", &x, &y, &ra, &dec) < 4)
			break;
		if (x == -1 && y == -1) {
			dl_append(otherradecs, ra);
			dl_append(otherradecs, dec);
		} else {
			dl_append(xys, x);
			dl_append(xys, y);
			dl_append(radecs, ra);
			dl_append(radecs, dec);
		}
	}
	logmsg("Read %i x,y,ra,dec tuples\n", dl_size(xys)/2);

	N = dl_size(xys)/2;
	xy = dl_to_array(xys);
	xyz = malloc(3 * N * sizeof(double));
	for (i=0; i<N; i++)
		radecdeg2xyzarr(dl_get(radecs, 2*i), dl_get(radecs, 2*i+1), xyz + i*3);
	dl_free(xys);
	dl_free(radecs);

	blind_wcs_compute(xyz, xy, N, &tan, NULL);
	tan.imagew = W;
	tan.imageh = H;

	logmsg("Computed TAN WCS:\n");
	tan_print_to(&tan, logstream);

	for (i=0; i<dl_size(otherradecs)/2; i++) {
		double ra, dec, x,y;
		ra = dl_get(otherradecs, 2*i);
		dec = dl_get(otherradecs, 2*i+1);
		if (!tan_radec2pixelxy(&tan, ra, dec, &x, &y)) {
			logerr("Not in tangent plane: %g,%g\n", ra, dec);
			exit(-1);
			//continue;
		}
		printf("%g %g\n", x, y);
	}

	{
		tweak_t* t = tweak_new();
		starxy_t* sxy = starxy_new(N, FALSE, FALSE);
		il* imginds = il_new(256);
		il* refinds = il_new(256);
		//sip_t sip;

		for (i=0; i<N; i++) {
			starxy_set_x(sxy, i, xy[2*i+0]);
			starxy_set_y(sxy, i, xy[2*i+1]);
		}
		tweak_init(t);
		tweak_push_ref_xyz(t, xyz, N);
		tweak_push_image_xy(t, sxy);
		for (i=0; i<N; i++) {
			il_append(imginds, i);
			il_append(refinds, i);
		}
		// unweighted; no dist2s
		tweak_push_correspondence_indices(t, imginds, refinds, NULL, NULL);
		tweak_push_wcs_tan(t, &tan);
		t->sip->a_order = t->sip->b_order = t->sip->ap_order = t->sip->bp_order = 2;

		for (i=0; i<10; i++) {
			// go to TWEAK_HAS_LINEAR_CD -> do_sip_tweak
			// t->image has the indices of corresponding image stars
			// t->ref   has the indices of corresponding catalog stars
			tweak_go_to(t, TWEAK_HAS_LINEAR_CD);
			logmsg("\n");
			sip_print(t->sip);
			t->state &= ~TWEAK_HAS_LINEAR_CD;
		}
		tan_write_to_file(&t->sip->wcstan, "kt1.wcs");
	}

	 */
	return 0;
}



