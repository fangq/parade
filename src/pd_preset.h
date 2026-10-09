/* Office's preset shapes worked out (pd_preset.c): for the DOCX reader, which draws them */
#ifndef PD_PRESET_H
#define PD_PRESET_H

#define PD_PRESET_MAXPATH 16
#define PD_PRESET_MAXPT 4096

/* a preset's paths flattened in its box (0..w by 0..h): each path's points (NAN pairs between its rings) */
typedef struct {
    int npath;
    struct {
        int start, n;       /* its points in xy */
        int closed;         /* a ring of it closed */
        char fill[16];      /* norm, none, darken, darkenLess, lighten, lightenLess */
        int stroke;         /* outlined */
    } path[PD_PRESET_MAXPATH];
    double xy[2 * PD_PRESET_MAXPT];
    int n;
} pd_preset_flat;

/* whether NAME is one of Office's presets */
int pd_preset_known(const char* name);
/* the preset at a size with its adjustments ("adj1=5000 adj2=200"; those not given at their defaults); 0 for
   none */
int pd_preset_flatten(const char* name, double w, double h, const char* adj, pd_preset_flat* out);

/* where the preset's text goes in its box (Office's text rectangle); 0 for none */
int pd_preset_text_rect(const char* name, double w, double h, const char* adj, double* l, double* t, double* r,
                        double* b);

#endif
