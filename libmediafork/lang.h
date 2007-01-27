/* $Id: lang.h,v 1.1 2004/08/02 07:19:05 titer Exp $

   This file is part of the HandBrake source code.
   Homepage: <http://handbrake.m0k.org/>.
   It may be used under the terms of the GNU General Public License. */

#ifndef HB_LANG_H
#define HB_LANG_H

typedef struct iso639_lang_t
{
    char * eng_name;        /* Description in English */
    char * native_name;     /* Description in native language */
    char * iso639_1;       /* ISO-639-1 (2 characters) code */

} iso639_lang_t;

static const iso639_lang_t languages[] =
{ { "Afar", "", "aa" },
  { "Abkhazian", "", "ab" },
  { "Afrikaans", "", "af" },
  { "Albanian", "", "sq" },
  { "Amharic", "", "am" },
  { "Arabic", "", "ar" },
  { "Armenian", "", "hy" },
  { "Assamese", "", "as" },
  { "Avestan", "", "ae" },
  { "Aymara", "", "ay" },
  { "Azerbaijani", "", "az" },
  { "Bashkir", "", "ba" },
  { "Basque", "", "eu" },
  { "Belarusian", "", "be" },
  { "Bengali", "", "bn" },
  { "Bihari", "", "bh" },
  { "Bislama", "", "bi" },
  { "Bosnian", "", "bs" },
  { "Breton", "", "br" },
  { "Bulgarian", "", "bg" },
  { "Burmese", "", "my" },
  { "Catalan", "", "ca" },
  { "Chamorro", "", "ch" },
  { "Chechen", "", "ce" },
  { "Chinese", "", "zh" },
  { "Church Slavic", "", "cu" },
  { "Chuvash", "", "cv" },
  { "Cornish", "", "kw" },
  { "Corsican", "", "co" },
  { "Czech", "", "cs" },
  { "Danish", "Dansk", "da" },
  { "Dutch", "Nederlands", "nl" },
  { "Dzongkha", "", "dz" },
  { "English", "English", "en" },
  { "Esperanto", "", "eo" },
  { "Estonian", "", "et" },
  { "Faroese", "", "fo" },
  { "Fijian", "", "fj" },
  { "Finnish", "Suomi", "fi" },
  { "French", "Francais", "fr" },
  { "Frisian", "", "fy" },
  { "Georgian", "", "ka" },
  { "German", "Deutsch", "de" },
  { "Gaelic (Scots)", "", "gd" },
  { "Irish", "", "ga" },
  { "Gallegan", "", "gl" },
  { "Manx", "", "gv" },
  { "Greek, Modern ()", "", "el" },
  { "Guarani", "", "gn" },
  { "Gujarati", "", "gu" },
  { "Hebrew", "", "he" },
  { "Herero", "", "hz" },
  { "Hindi", "", "hi" },
  { "Hiri Motu", "", "ho" },
  { "Hungarian", "Magyar", "hu" },
  { "Icelandic", "Islenska", "is" },
  { "Inuktitut", "", "iu" },
  { "Interlingue", "", "ie" },
  { "Interlingua", "", "ia" },
  { "Indonesian", "", "id" },
  { "Inupiaq", "", "ik" },
  { "Italian", "Italiano", "it" },
  { "Javanese", "", "jv" },
  { "Japanese", "", "ja" },
  { "Kalaallisut (Greenlandic)", "", "kl" },
  { "Kannada", "", "kn" },
  { "Kashmiri", "", "ks" },
  { "Kazakh", "", "kk" },
  { "Khmer", "", "km" },
  { "Kikuyu", "", "ki" },
  { "Kinyarwanda", "", "rw" },
  { "Kirghiz", "", "ky" },
  { "Komi", "", "kv" },
  { "Korean", "", "ko" },
  { "Kuanyama", "", "kj" },
  { "Kurdish", "", "ku" },
  { "Lao", "", "lo" },
  { "Latin", "", "la" },
  { "Latvian", "", "lv" },
  { "Lingala", "", "ln" },
  { "Lithuanian", "", "lt" },
  { "Letzeburgesch", "", "lb" },
  { "Macedonian", "", "mk" },
  { "Marshall", "", "mh" },
  { "Malayalam", "", "ml" },
  { "Maori", "", "mi" },
  { "Marathi", "", "mr" },
  { "Malay", "", "ms" },
  { "Malagasy", "", "mg" },
  { "Maltese", "", "mt" },
  { "Moldavian", "", "mo" },
  { "Mongolian", "", "mn" },
  { "Nauru", "", "na" },
  { "Navajo", "", "nv" },
  { "Ndebele, South", "", "nr" },
  { "Ndebele, North", "", "nd" },
  { "Ndonga", "", "ng" },
  { "Nepali", "", "ne" },
  { "Norwegian", "Norsk", "no" },
  { "Norwegian Nynorsk", "", "nn" },
  { "Norwegian Bokmål", "", "nb" },
  { "Chichewa; Nyanja", "", "ny" },
  { "Occitan (post 1500); Provençal", "", "oc" },
  { "Oriya", "", "or" },
  { "Oromo", "", "om" },
  { "Ossetian; Ossetic", "", "os" },
  { "Panjabi", "", "pa" },
  { "Persian", "", "fa" },
  { "Pali", "", "pi" },
  { "Polish", "", "pl" },
  { "Portuguese", "Portugues", "pt" },
  { "Pushto", "", "ps" },
  { "Quechua", "", "qu" },
  { "Raeto-Romance", "", "rm" },
  { "Romanian", "", "ro" },
  { "Rundi", "", "rn" },
  { "Russian", "", "ru" },
  { "Sango", "", "sg" },
  { "Sanskrit", "", "sa" },
  { "Serbian", "", "sr" },
  { "Croatian", "Hrvatski", "hr" },
  { "Sinhalese", "", "si" },
  { "Slovak", "", "sk" },
  { "Slovenian", "", "sl" },
  { "Northern Sami", "", "se" },
  { "Samoan", "", "sm" },
  { "Shona", "", "sn" },
  { "Sindhi", "", "sd" },
  { "Somali", "", "so" },
  { "Sotho, Southern", "", "st" },
  { "Spanish", "Espanol", "es" },
  { "Sardinian", "", "sc" },
  { "Swati", "", "ss" },
  { "Sundanese", "", "su" },
  { "Swahili", "", "sw" },
  { "Swedish", "Svenska", "sv" },
  { "Tahitian", "", "ty" },
  { "Tamil", "", "ta" },
  { "Tatar", "", "tt" },
  { "Telugu", "", "te" },
  { "Tajik", "", "tg" },
  { "Tagalog", "", "tl" },
  { "Thai", "", "th" },
  { "Tibetan", "", "bo" },
  { "Tigrinya", "", "ti" },
  { "Tonga (Tonga Islands)", "", "to" },
  { "Tswana", "", "tn" },
  { "Tsonga", "", "ts" },
  { "Turkish", "", "tr" },
  { "Turkmen", "", "tk" },
  { "Twi", "", "tw" },
  { "Uighur", "", "ug" },
  { "Ukrainian", "", "uk" },
  { "Urdu", "", "ur" },
  { "Uzbek", "", "uz" },
  { "Vietnamese", "", "vi" },
  { "Volapük", "", "vo" },
  { "Welsh", "", "cy" },
  { "Wolof", "", "wo" },
  { "Xhosa", "", "xh" },
  { "Yiddish", "", "yi" },
  { "Yoruba", "", "yo" },
  { "Zhuang", "", "za" },
  { "Zulu", "", "zu" },
  { NULL, NULL, NULL } };

static char * lang_for_code( int code )
{
    char code_string[2];
    iso639_lang_t * lang;

    code_string[0] = ( code >> 8 ) & 0xFF;
    code_string[1] = code & 0xFF;

    for( lang = (iso639_lang_t*) languages; lang->eng_name; lang++ )
    {
        if( !strncmp( lang->iso639_1, code_string, 2 ) )
        {
            if( *lang->native_name )
            {
                return lang->native_name;
            }

            return lang->eng_name;
        }
    }

    return "Unknown";
}

#endif
