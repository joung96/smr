#ifndef FONTMAGIC
/* Font file layout */


/************************************\
* 				     *
* 	James Gosling, 1983	     *
* 	Copyright (c) 1983 IBM	     *
* 	IBM/ITC Internal use only    *
* 				     *
\************************************/

#define CharsPerFont 128

/**************************************************\
* 						   *
* An icon is a patch that can be drawn.  It has a  *
* distinguished point called its origin.	   *
* 						   *
* 	           North			   *
*         |---------WtoE----------->|		   *
*      -  +-------------------------+		   *
*      |  |          /             /|		   *
*      |  |         /             / |		   *
*      |  |        /             /  |		   *
*      |  |       /             /   |		   *
*      N  |      /             /    |	East	   *
*      t  |     /             /     |		   *
*      o  |    /             /      |		   *
*      S  |---*-------------/-------|		   *
*      |  |  / Origin      /        |		   *
*      |  | /             /         |		   *
*      V  |/             /          |		   *
*      -  +-------------------------+		   *
*         <---|---Spacing-->|			   *
*           ^Wbase				   *
* 	        South				   *
* 						   *
\**************************************************/

struct SVector {		/* Short Vector */
    short x, y;
};

/* Given a pointer to an icon, GenericPart returns a pointer to its IconGenericPart */
#define GenericPart(icon) ((struct IconGenericPart *) (((int) (icon)) + (icon) -> OffsetToGeneric))

/* Given a character and a font, GenericPartOfChar returns the corresponding IconGenericPart */
#define GenericPartOfChar(f,c) GenericPart(&((f)->chars[c]))

struct IconGenericPart {	/* information relating to this icon that
				   is of general interest */
    struct SVector  Spacing,	/* The vector which when added to the
				   origin of this character will give the
				   origin of the next character to follow
				   it */
                    NWtoOrigin,	/* Vector from the origin to the North
				   West corner of the characters bounding
				   box */
                    NtoS,	/* North to south vector */
                    WtoE,	/* West to East vector */
                    Wbase;	/* Vector from the origin to the West edge
				   parallel to the baseline */
};

struct BitmapIconSpecificPart {	/* information relating to an icon that is
				   necessary only if you intend to draw it 
				*/
    char    type;		/* The type of representation used for
				   this icon.  (= BitmapIcon) */
    unsigned char   rows,	/* rows and columns in this bitmap */
                    cols;
    char    orow,		/* row and column of the origin */
            ocol;		/* Note that these are signed */
    unsigned short  bits[1];	/* The bitmap associated with this icon */
};

struct icon {			/* An icon is made up of a generic and a
				   specific part.  The address of each
				   is "Offset" bytes from the "icon"
				   structure */
    short   OffsetToGeneric,
            OffsetToSpecific;
};


/* A font name description block.  These are used in font definitions and in
   font directories */
struct FontName {
    char    FamilyName[16];	/* eg. "TimesRoman" */
    short   rotation;		/* The rotation of this font (degrees;
				   +ve=>clockwise) */
    char    height;		/* font height in points */
    char    FaceCode;		/* eg. "Italic" or "Bold" or "Bold Italic"
				   */
};

/* Possible icon types: */
#define AssortedIcon 0		/* Not used in icons, only in fonts: the
				   icons have an assortment of types */
#define BitmapIcon 1		/* The icon is represented by a bitmap */
#define VectorIcon 2		/* The icon is represented as vectors */


/* A font.  This structure is at the front of every font file.  The icon
   generic and specific parts follow.  They are pointed to by offsets in
   the icon structures */
struct font {
    short   magic;		/* used to detect invalid font files */
    short   NonSpecificLength;	/* number of bytes in the font and
				   generic parts */
    struct FontName fn;		/* The name of this font */
    struct SVector  NWtoOrigin,	/* These are "maximal" versions of the
				   variables by the same names in each
				   constituent icon */
                    NtoS,	/* North to South */
                    WtoE,	/* West to East */
		    Wbase,	/* From the origin along the baseline to
				   the West edge */
                    newline;	/* The appropriate "newline" vector, its
				   just NtoS with an appropriate fudge
				   factor added */
    char    type;		/* The type of representation used for the
				   icons within this font.  If all icons
				   within the font share the same type,
				   then type is that type, otherwise it is
				   "AssortedIcon" */
    short  NIcons;		/* The number of icons actually in this font.
				   The constant "CharsPerFont" doesn't
				   actually restrict the size of the
				   following array; it's just used to specify
				   the local common case */
    struct icon    chars[CharsPerFont];
 /* at the end of the font structure come the bits for each character */
};

/* The value of font->magic is set to FONTMAGIC.  This is used to check that
   a file does indeed contain a font */
#define FONTMAGIC 0x1fd

/* FaceCode flags */
#define BoldFace 1
#define ItalicFace 2
#define ShadowFace 4
#define FixedWidthFace 010

struct font *getpfont();	/* get font given name to parse */
struct font *getfont();		/* get font given parsed name */
struct icon *geticon();		/* get an icon given a font and a slot */
int LastX, LastY;		/* coordinates following the end of
				   the last string drawn */
#endif
