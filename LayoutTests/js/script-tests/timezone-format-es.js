const date1 = new Date(1651263043690);

shouldBeEqualToString("date1.toString()", "Fri Apr 29 2022 22:10:43 GMT+0200 (hora de verano de Europa central)");
shouldBeEqualToString("date1.toTimeString()", "22:10:43 GMT+0200 (hora de verano de Europa central)");
shouldBeEqualToString("Intl.DateTimeFormat().resolvedOptions().timeZone", "Europe/Madrid");
