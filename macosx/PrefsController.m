#import "PrefsController.h"

@implementation PrefsController

- (void) awakeFromNib
{
    NSUserDefaults * defaults;
    NSDictionary   * appDefaults;
    
    /* Unless the user specified otherwise, default is to check
       for update */
    defaults    = [NSUserDefaults standardUserDefaults];
    appDefaults = [NSDictionary dictionaryWithObject:@"YES"
                   forKey:@"CheckForUpdates"];
	appDefaults = [NSDictionary dictionaryWithObject:@"English"
                   forKey:@"DefaultLanguage"];
    [defaults registerDefaults: appDefaults];

    /* Check or uncheck according to the preferences */
    [fUpdateCheck setState: [defaults boolForKey:@"CheckForUpdates"] ?
        NSOnState : NSOffState];
	// Fill the languages combobox
    [fdefaultlanguage removeAllItems];
	[fdefaultlanguage addItemWithObjectValue: @"Afar"];
	[fdefaultlanguage addItemWithObjectValue: @"Abkhazian"];
	[fdefaultlanguage addItemWithObjectValue: @"Afrikaans"];
	[fdefaultlanguage addItemWithObjectValue: @"Albanian"];
	[fdefaultlanguage addItemWithObjectValue: @"Amharic"];
	[fdefaultlanguage addItemWithObjectValue: @"Arabic"];
	[fdefaultlanguage addItemWithObjectValue: @"Armenian"];
	[fdefaultlanguage addItemWithObjectValue: @"Assamese"];
	[fdefaultlanguage addItemWithObjectValue: @"Avestan"];
	[fdefaultlanguage addItemWithObjectValue: @"Aymara"];
	[fdefaultlanguage addItemWithObjectValue: @"Azerbaijani"];
	[fdefaultlanguage addItemWithObjectValue: @"Bashkir"];
	[fdefaultlanguage addItemWithObjectValue: @"Basque"];
	[fdefaultlanguage addItemWithObjectValue: @"Belarusian"];
	[fdefaultlanguage addItemWithObjectValue: @"Bengali"];
	[fdefaultlanguage addItemWithObjectValue: @"Bihari"];
	[fdefaultlanguage addItemWithObjectValue: @"Bislama"];
	[fdefaultlanguage addItemWithObjectValue: @"Bosnian"];
	[fdefaultlanguage addItemWithObjectValue: @"Breton"];
	[fdefaultlanguage addItemWithObjectValue: @"Bulgarian"];
	[fdefaultlanguage addItemWithObjectValue: @"Burmese"];
	[fdefaultlanguage addItemWithObjectValue: @"Catalan"];
	[fdefaultlanguage addItemWithObjectValue: @"Chamorro"];
	[fdefaultlanguage addItemWithObjectValue: @"Chechen"];
	[fdefaultlanguage addItemWithObjectValue: @"Chichewa; Nyanja"];
	[fdefaultlanguage addItemWithObjectValue: @"Chinese"];
	[fdefaultlanguage addItemWithObjectValue: @"Church Slavic"];
	[fdefaultlanguage addItemWithObjectValue: @"Chuvash"];
	[fdefaultlanguage addItemWithObjectValue: @"Cornish"];
	[fdefaultlanguage addItemWithObjectValue: @"Corsican"];
	[fdefaultlanguage addItemWithObjectValue: @"Croatian"];
	[fdefaultlanguage addItemWithObjectValue: @"Czech"];
	[fdefaultlanguage addItemWithObjectValue: @"Dansk"];
	[fdefaultlanguage addItemWithObjectValue: @"Deutsch"];
	[fdefaultlanguage addItemWithObjectValue: @"Dzongkha"];
	[fdefaultlanguage addItemWithObjectValue: @"English"];
	[fdefaultlanguage addItemWithObjectValue: @"Espanol"];
	[fdefaultlanguage addItemWithObjectValue: @"Esperanto"];
	[fdefaultlanguage addItemWithObjectValue: @"Estonian"];
	[fdefaultlanguage addItemWithObjectValue: @"Faroese"];
	[fdefaultlanguage addItemWithObjectValue: @"Fijian"];
	[fdefaultlanguage addItemWithObjectValue: @"Francais"];
	[fdefaultlanguage addItemWithObjectValue: @"Frisian"];
	[fdefaultlanguage addItemWithObjectValue: @"Georgian"];
	[fdefaultlanguage addItemWithObjectValue: @"Gaelic (Scots)"];
	[fdefaultlanguage addItemWithObjectValue: @"Gallegan"];
	[fdefaultlanguage addItemWithObjectValue: @"Greek, Modern ()"];
	[fdefaultlanguage addItemWithObjectValue: @"Guarani"];
	[fdefaultlanguage addItemWithObjectValue: @"Gujarati"];
	[fdefaultlanguage addItemWithObjectValue: @"Hebrew"];
	[fdefaultlanguage addItemWithObjectValue: @"Herero"];
	[fdefaultlanguage addItemWithObjectValue: @"Hindi"];
	[fdefaultlanguage addItemWithObjectValue: @"Hiri Motu"];
	[fdefaultlanguage addItemWithObjectValue: @"Inuktitut"];
	[fdefaultlanguage addItemWithObjectValue: @"Interlingue"];
	[fdefaultlanguage addItemWithObjectValue: @"Interlingua"];
	[fdefaultlanguage addItemWithObjectValue: @"Indonesian"];
	[fdefaultlanguage addItemWithObjectValue: @"Inupiaq"];
	[fdefaultlanguage addItemWithObjectValue: @"Irish"];
	[fdefaultlanguage addItemWithObjectValue: @"Islenska"];
	[fdefaultlanguage addItemWithObjectValue: @"Italian"];
	[fdefaultlanguage addItemWithObjectValue: @"Javanese"];
	[fdefaultlanguage addItemWithObjectValue: @"Japanese"];
	[fdefaultlanguage addItemWithObjectValue: @"Kalaallisut (Greenlandic)"];
	[fdefaultlanguage addItemWithObjectValue: @"Kannada"];
	[fdefaultlanguage addItemWithObjectValue: @"Kashmiri"];
	[fdefaultlanguage addItemWithObjectValue: @"Kazakh"];
	[fdefaultlanguage addItemWithObjectValue: @"Khmer"];
	[fdefaultlanguage addItemWithObjectValue: @"Kikuyu"];
	[fdefaultlanguage addItemWithObjectValue: @"Kinyarwanda"];
	[fdefaultlanguage addItemWithObjectValue: @"Kirghiz"];
	[fdefaultlanguage addItemWithObjectValue: @"Komi"];
	[fdefaultlanguage addItemWithObjectValue: @"Korean"];
	[fdefaultlanguage addItemWithObjectValue: @"Kuanyama"];
	[fdefaultlanguage addItemWithObjectValue: @"Kurdish"];
	[fdefaultlanguage addItemWithObjectValue: @"Lao"];
	[fdefaultlanguage addItemWithObjectValue: @"Latin"];
	[fdefaultlanguage addItemWithObjectValue: @"Latvian"];
	[fdefaultlanguage addItemWithObjectValue: @"Lingala"];
	[fdefaultlanguage addItemWithObjectValue: @"Lithuanian"];
	[fdefaultlanguage addItemWithObjectValue: @"Letzeburgesch"];
	[fdefaultlanguage addItemWithObjectValue: @"Macedonian"];
	[fdefaultlanguage addItemWithObjectValue: @"Magyar"];
	[fdefaultlanguage addItemWithObjectValue: @"Malay"];
	[fdefaultlanguage addItemWithObjectValue: @"Malayalam"];
	[fdefaultlanguage addItemWithObjectValue: @"Malagasy"];
	[fdefaultlanguage addItemWithObjectValue: @"Maltese"];
	[fdefaultlanguage addItemWithObjectValue: @"Manx"];
	[fdefaultlanguage addItemWithObjectValue: @"Maori"];
	[fdefaultlanguage addItemWithObjectValue: @"Marathi"];
	[fdefaultlanguage addItemWithObjectValue: @"Marshall"];
	[fdefaultlanguage addItemWithObjectValue: @"Moldavian"];
	[fdefaultlanguage addItemWithObjectValue: @"Mongolian"];
	[fdefaultlanguage addItemWithObjectValue: @"Nauru"];
	[fdefaultlanguage addItemWithObjectValue: @"Navajo"];
	[fdefaultlanguage addItemWithObjectValue: @"Ndebele, South"];
	[fdefaultlanguage addItemWithObjectValue: @"Ndebele, North"];
	[fdefaultlanguage addItemWithObjectValue: @"Ndonga"];
	[fdefaultlanguage addItemWithObjectValue: @"Nederlands"];
	[fdefaultlanguage addItemWithObjectValue: @"Nepali"];
	[fdefaultlanguage addItemWithObjectValue: @"Northern Sami"];
	[fdefaultlanguage addItemWithObjectValue: @"Norwegian"];
	[fdefaultlanguage addItemWithObjectValue: @"Norwegian Bokmal"];
	[fdefaultlanguage addItemWithObjectValue: @"Norwegian Nynorsk"];
	[fdefaultlanguage addItemWithObjectValue: @"Occitan (post 1500); Provencal"];
	[fdefaultlanguage addItemWithObjectValue: @"Oriya"];
	[fdefaultlanguage addItemWithObjectValue: @"Oromo"];
	[fdefaultlanguage addItemWithObjectValue: @"Ossetian; Ossetic"];
	[fdefaultlanguage addItemWithObjectValue: @"Panjabi"];
	[fdefaultlanguage addItemWithObjectValue: @"Persian"];
	[fdefaultlanguage addItemWithObjectValue: @"Pali"];
	[fdefaultlanguage addItemWithObjectValue: @"Polish"];
	[fdefaultlanguage addItemWithObjectValue: @"Portugues"];
	[fdefaultlanguage addItemWithObjectValue: @"Pushto"];
	[fdefaultlanguage addItemWithObjectValue: @"Quechua"];
	[fdefaultlanguage addItemWithObjectValue: @"Raeto-Romance"];
	[fdefaultlanguage addItemWithObjectValue: @"Romanian"];
	[fdefaultlanguage addItemWithObjectValue: @"Rundi"];
	[fdefaultlanguage addItemWithObjectValue: @"Russian"];
	[fdefaultlanguage addItemWithObjectValue: @"Sango"];
	[fdefaultlanguage addItemWithObjectValue: @"Sanskrit"];
	[fdefaultlanguage addItemWithObjectValue: @"Sardinian"];
	[fdefaultlanguage addItemWithObjectValue: @"Serbian"];
	[fdefaultlanguage addItemWithObjectValue: @"Sinhalese"];
	[fdefaultlanguage addItemWithObjectValue: @"Slovak"];
	[fdefaultlanguage addItemWithObjectValue: @"Slovenian"];
	[fdefaultlanguage addItemWithObjectValue: @"Samoan"];
	[fdefaultlanguage addItemWithObjectValue: @"Shona"];
	[fdefaultlanguage addItemWithObjectValue: @"Sindhi"];
	[fdefaultlanguage addItemWithObjectValue: @"Somali"];
	[fdefaultlanguage addItemWithObjectValue: @"Sotho, Southern"];
	[fdefaultlanguage addItemWithObjectValue: @"Sundanese"];
	[fdefaultlanguage addItemWithObjectValue: @"Suomi"];
	[fdefaultlanguage addItemWithObjectValue: @"Svenska"];
	[fdefaultlanguage addItemWithObjectValue: @"Swahili"];
	[fdefaultlanguage addItemWithObjectValue: @"Swati"];
	[fdefaultlanguage addItemWithObjectValue: @"Tahitian"];
	[fdefaultlanguage addItemWithObjectValue: @"Tamil"];
	[fdefaultlanguage addItemWithObjectValue: @"Tatar"];
	[fdefaultlanguage addItemWithObjectValue: @"Telugu"];
	[fdefaultlanguage addItemWithObjectValue: @"Tajik"];
	[fdefaultlanguage addItemWithObjectValue: @"Tagalog"];
	[fdefaultlanguage addItemWithObjectValue: @"Thai"];
	[fdefaultlanguage addItemWithObjectValue: @"Tibetan"];
	[fdefaultlanguage addItemWithObjectValue: @"Tigrinya"];
	[fdefaultlanguage addItemWithObjectValue: @"Tonga (Tonga Islands)"];
	[fdefaultlanguage addItemWithObjectValue: @"Tswana"];
	[fdefaultlanguage addItemWithObjectValue: @"Tsonga"];
	[fdefaultlanguage addItemWithObjectValue: @"Turkish"];
	[fdefaultlanguage addItemWithObjectValue: @"Turkmen"];
	[fdefaultlanguage addItemWithObjectValue: @"Twi"];
	[fdefaultlanguage addItemWithObjectValue: @"Uighur"];
	[fdefaultlanguage addItemWithObjectValue: @"Ukrainian"];
	[fdefaultlanguage addItemWithObjectValue: @"Urdu"];
	[fdefaultlanguage addItemWithObjectValue: @"Uzbek"];
	[fdefaultlanguage addItemWithObjectValue: @"Vietnamese"];
	[fdefaultlanguage addItemWithObjectValue: @"Volapk"];
	[fdefaultlanguage addItemWithObjectValue: @"Welsh"];
	[fdefaultlanguage addItemWithObjectValue: @"Wolof"];
	[fdefaultlanguage addItemWithObjectValue: @"Xhosa"];
	[fdefaultlanguage addItemWithObjectValue: @"Yiddish"];
	[fdefaultlanguage addItemWithObjectValue: @"Yoruba"];
	[fdefaultlanguage addItemWithObjectValue: @"Zhuang"];
	[fdefaultlanguage addItemWithObjectValue: @"Zulu"];
	
	[fdefaultlanguage setStringValue:[defaults stringForKey:@"DefaultLanguage"]];
    [fdefaultlanguage selectItemWithObjectValue:[defaults stringForKey:@"DefaultLanguage"]];

}


- (IBAction) OpenPanel: (id) sender;
{
    [NSApp runModalForWindow: fPanel];
}

- (IBAction) ClosePanel: (id) sender;
{
    [NSApp stopModal];
    [fPanel orderOut: sender];
}

- (IBAction) CheckChanged: (id) sender
{
    NSUserDefaults * defaults = [NSUserDefaults standardUserDefaults];
    
    if( [fUpdateCheck state] == NSOnState )
    {
        [defaults setObject:@"YES" forKey:@"CheckForUpdates"];
    }
    else
    {
        [defaults setObject:@"NO" forKey:@"CheckForUpdates"];
    }
	
	[defaults setObject:[fdefaultlanguage objectValueOfSelectedItem]  forKey:@"DefaultLanguage"];

}

@end
