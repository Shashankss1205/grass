/*!
   \file vector/Vlib/open_ogr.c

   \brief Vector library - Open OGR layer as vector map layer

   Higher level functions for reading/writing/manipulating vectors.

   (C) 2001-2010 by the GRASS Development Team

   This program is free software under the GNU General Public License
   (>=v2). Read the file COPYING that comes with GRASS for details.

   \author Original author CERL, probably Dave Gerdes or Mike Higgins.
   \author Update to GRASS 5.7 Radim Blazek and David D. Gray.
   \author Update to GRASS 7.0 Martin Landa <landa.martin gmail.com> (2009)
 */

#include <grass/config.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <grass/vector.h>
#include <grass/gis.h>
#include <grass/glocale.h>

#ifdef HAVE_OGR
#include <ogr_api.h>
#include <cpl_string.h>

/*!
   \brief Open existing OGR layer (level 1 - without feature index file)

   Map->name, Map->mapset, Map->fInfo.ogr.dsn and
   Map->fInfo.ogr.layer_name must be set before.

   \param[in,out] Map pointer to Map_info structure
   \param update non-zero for write mode, otherwise read-only
   (write mode is currently not supported)
   
   \return 0 success
   \return -1 error
*/
int V1_open_old_ogr(struct Map_info *Map, int update)
{
    int i, layer, nLayers;
    OGRDataSourceH Ogr_ds;
    OGRLayerH Ogr_layer;
    OGRFeatureDefnH Ogr_featuredefn;

    Ogr_layer = NULL;

    /*
    if (update) {
	G_warning(_("Write mode is not supported for OGR format"));
	return -1;
    }
    */
    
    if (!Map->fInfo.ogr.dsn) {
	G_fatal_error(_("OGR datasource not defined"));
	return -1;
    }
    
    if (!Map->fInfo.ogr.layer_name) {
	G_fatal_error(_("OGR layer not defined"));
	return -1;
    }
    
    G_debug(2, "V1_open_old_ogr(): dsn = %s layer = %s", Map->fInfo.ogr.dsn,
	    Map->fInfo.ogr.layer_name);

    OGRRegisterAll();

    /* open data source handle */
    Ogr_ds = OGROpen(Map->fInfo.ogr.dsn, FALSE, NULL);
    if (Ogr_ds == NULL)
	G_fatal_error(_("Unable to open OGR data source '%s'"),
		      Map->fInfo.ogr.dsn);
    Map->fInfo.ogr.ds = Ogr_ds;

    /* get layer number */
    layer = -1;
    nLayers = OGR_DS_GetLayerCount(Ogr_ds);
    G_debug(2, "%d layers found in data source", nLayers);

    for (i = 0; i < nLayers; i++) {
	Ogr_layer = OGR_DS_GetLayer(Ogr_ds, i);
	Ogr_featuredefn = OGR_L_GetLayerDefn(Ogr_layer);
	if (strcmp(OGR_FD_GetName(Ogr_featuredefn), Map->fInfo.ogr.layer_name) == 0) {
	    layer = i;
	    break;
	}
    }
    if (layer == -1) {
	OGR_DS_Destroy(Ogr_ds);
	G_fatal_error(_("OGR layer <%s> not found"),
		      Map->fInfo.ogr.layer_name);
    }
    G_debug(2, "OGR layer %d opened", layer);

    Map->fInfo.ogr.layer = Ogr_layer;
    
    Map->fInfo.ogr.lines = NULL;
    Map->fInfo.ogr.lines_types = NULL;
    Map->fInfo.ogr.lines_alloc = 0;
    Map->fInfo.ogr.lines_num = 0;
    Map->fInfo.ogr.lines_next = 0;
    
    Map->head.with_z = WITHOUT_Z;	/* TODO: 3D */
    
    Map->fInfo.ogr.feature_cache = NULL;
    Map->fInfo.ogr.feature_cache_id = -1;	/* FID >= 0 */
    
    return (0);
}

/*!
   \brief Open existing OGR layer (level 2 - feature index)

   \param[in,out] Map pointer to Map_info structure
   
   \return 0 success
   \return -1 error
*/
int V2_open_old_ogr(struct Map_info *Map)
{
    char elem[GPATH_MAX];
    char buf[5];		/* used for format version */
    long length;
    struct gvfile fp;
    struct Port_info port;
    int Version_Major, Version_Minor, Back_Major, Back_Minor, byte_order;

    G_debug(3, "V2_open_old_ogr()");
    
    sprintf(elem, "%s/%s", GV_DIRECTORY, Map->name);
    dig_file_init(&fp);
    fp.file = G_fopen_old(elem, GV_FIDX_ELEMENT, Map->mapset);
    if (fp.file == NULL) {
	G_warning(_("Unable to open fidx file for vector map <%s@%s>"),
		  Map->name, Map->mapset);
	return -1;
    }

    /* Header */
    if (0 >= dig__fread_port_C(buf, 5, &fp))
	return (-1);
    Version_Major = buf[0];
    Version_Minor = buf[1];
    Back_Major = buf[2];
    Back_Minor = buf[3];
    byte_order = buf[4];
    
    /* check version numbers */
    if (Version_Major > 5 || Version_Minor > 0) {
	if (Back_Major > 5 || Back_Minor > 0) {
	    G_fatal_error(_("Feature index format version %d.%d is not supported by this release."
			   " Try to rebuild topology or upgrade GRASS."),
			  Version_Major, Version_Minor);
	    return (-1);
	}
	G_warning(_("Your GRASS version does not fully support feature index format %d.%d of the vector."
		   " Consider to rebuild topology or upgrade GRASS."),
		  Version_Major, Version_Minor);
    }

    dig_init_portable(&port, byte_order);
    dig_set_cur_port(&port);

    /* Body */
    /* bytes 6 - 9 : header size */
    if (0 >= dig__fread_port_L(&length, 1, &fp))
	return (-1);
    G_debug(3, "  header size %ld", length);

    G_fseek(fp.file, length, SEEK_SET);

    /* number of records  */
    if (0 >= dig__fread_port_I(&(Map->fInfo.ogr.offset_num), 1, &fp))
	return (-1);

    /* alloc space */
    Map->fInfo.ogr.offset =
	(int *)G_malloc(Map->fInfo.ogr.offset_num * sizeof(int));
    Map->fInfo.ogr.offset_alloc = Map->fInfo.ogr.offset_num;

    /* offsets */
    if (0 >= dig__fread_port_I(Map->fInfo.ogr.offset,
			       Map->fInfo.ogr.offset_num, &fp))
	return (-1);

    fclose(fp.file);

    G_debug(3, "%d records read from fidx", Map->fInfo.ogr.offset_num);

    Map->fInfo.ogr.next_line = 1;

    return 0;
}

/*!
   \brief Create new OGR layer in given OGR datasource (level 1)

   \param[out] Map pointer to Map_info structure
   \param name name of OGR layer to create
   \param with_z 2D or 3D (unused?)

   \return 0 success
   \return -1 error 
*/
int V1_open_new_ogr(struct Map_info *Map, const char *name, int with_z)
{
    OGRSFDriverH   Ogr_driver;
    OGRDataSourceH Ogr_ds;
    OGRLayerH      Ogr_layer;
    
    int            i;
    char         **Ogr_layer_options;
     
    Ogr_layer_options = NULL;
    
    OGRRegisterAll();
	    
    Ogr_driver = OGRGetDriverByName(Map->fInfo.ogr.driver_name);
    if (!Ogr_driver) {
	G_warning(_("Unable to get OGR driver <%s>"), Map->fInfo.ogr.driver_name);
	return -1;
    }
    Map->fInfo.ogr.driver = Ogr_driver;
    
    /* TODO: creation options */
    Ogr_ds = OGR_Dr_CreateDataSource(Ogr_driver, Map->fInfo.ogr.dsn, NULL);
    if (!Ogr_ds) {
	G_warning(_("Unable to create OGR data source '%s'"),
		  Map->fInfo.ogr.dsn);
	return -1;
    }
    Map->fInfo.ogr.ds = Ogr_ds;

    Ogr_layer = OGR_DS_GetLayerByName(Ogr_ds, Map->fInfo.ogr.layer_name);
    if (Ogr_layer) {
	for (i = 0; i < OGR_DS_GetLayerCount(Ogr_ds); i++) {
	    if (OGR_DS_GetLayer(Ogr_ds, i) == Ogr_layer) {
		if (G_get_overwrite()) {
		    G_warning(_("OGR layer <%s> already exists and will be overwritten"),
		      Map->fInfo.ogr.layer_name);

		    if (OGR_DS_DeleteLayer(Ogr_ds, i) != OGRERR_NONE) {
			G_warning(_("Unable to delete OGR layer <%s>"),
				  Map->fInfo.ogr.layer_name);
			return -1;
		    }
		}
		else {
		    G_fatal_error(_("OGR layer <%s> already exists in datasource '%s'"),
				  Map->fInfo.ogr.layer_name, Map->fInfo.ogr.dsn);
		}
		break;
	    }
	}
    }
    
    /* create new OGR layer */
    /* TODO: spatial reference */
    /* Ogr_layer_options = CSLSetNameValue(Ogr_layer_options, "OVERWRITE", "YES"); */
    Ogr_layer = OGR_DS_CreateLayer(Ogr_ds, Map->fInfo.ogr.layer_name,
				   NULL, wkbPoint, Ogr_layer_options);
    CSLDestroy(Ogr_layer_options);
    if (!Ogr_layer) {
	G_warning(_("Unable to create OGR layer <%s> in '%s'"),
		  Map->fInfo.ogr.layer_name, Map->fInfo.ogr.dsn);
	return -1;
    }
    Map->fInfo.ogr.layer = Ogr_layer;
    
    return 0;
}
#endif
