module.exports = [
	{
		type: "heading",
		defaultValue: "T1000 CGM Settings"
	},
	{
		type: "section",
		items: [
			{
				type: "heading",
				defaultValue: "Dexcom Share Credentials"
			},
			{
				type: "input",
				messageKey: "accountName",
				label: "Username",
				attributes: {
					placeholder: "Dexcom Share username",
					autocapitalize: "off",
					autocorrect: "off"
				}
			},
			{
				type: "input",
				messageKey: "password",
				label: "Password",
				attributes: {
					placeholder: "Dexcom Share password",
					type: "password"
				}
			},
			{
				type: "select",
				messageKey: "server",
				label: "Server Region",
				defaultValue: "us",
				options: [
					{ label: "United States", value: "us" },
					{ label: "International (Outside US)", value: "international" }
				]
			}
		]
	},
	{
		type: "section",
		items: [
			{
				type: "heading",
				defaultValue: "Display Settings"
			},
			{
				type: "toggle",
				messageKey: "reversed",
				label: "Reversed (black on white)",
				defaultValue: false
			},
			{
				type: "select",
				messageKey: "unit",
				label: "Glucose Units",
				defaultValue: "mgdl",
				options: [
					{ label: "mg/dL", value: "mgdl" },
					{ label: "mmol/L", value: "mmol" }
				]
			},
			{
				type: "slider",
				messageKey: "lowThreshold",
				label: "Low Threshold",
				defaultValue: 70,
				min: 55,
				max: 100,
				step: 5
			},
			{
				type: "slider",
				messageKey: "highThreshold",
				label: "High Threshold",
				defaultValue: 180,
				min: 140,
				max: 300,
				step: 10
			},
			{
				type: "text",
				defaultValue: "<small>Threshold lines shown on the chart</small>"
			}
		]
	},
	{
		type: "section",
		items: [
			{
				type: "heading",
				defaultValue: "Low Soon Alert"
			},
			{
				type: "toggle",
				messageKey: "vibeLowSoonEnabled",
				label: "Enable low soon alert vibration",
				defaultValue: false
			},
			{
				type: "slider",
				messageKey: "vibeLowSoonThreshold",
				label: "Low Soon Threshold",
				defaultValue: 80,
				min: 55,
				max: 100,
				step: 5
			},
			{
				type: "text",
				defaultValue: "<small>Vibrate when predicted to be below this value in 20 minutes</small>"
			},
			{
				type: "slider",
				messageKey: "vibeLowSoonRepeatMinutes",
				label: "Repeat (minutes)",
				defaultValue: 30,
				min: 15,
				max: 60,
				step: 5
			}
		]
	},
	{
		type: "section",
		items: [
			{
				type: "heading",
				defaultValue: "High Alert"
			},
			{
				type: "toggle",
				messageKey: "vibeEnabled",
				label: "Enable high alert vibration",
				defaultValue: false
			},
			{
				type: "slider",
				messageKey: "vibeHighThreshold",
				label: "High Alert Threshold",
				defaultValue: 250,
				min: 200,
				max: 300,
				step: 10
			},
			{
				type: "text",
				defaultValue: "<small>Vibrate when glucose is above this value</small>"
			},
			{
				type: "slider",
				messageKey: "vibeDelayMinutes",
				label: "Delay (minutes)",
				defaultValue: 60,
				min: 15,
				max: 120,
				step: 15
			},
			{
				type: "slider",
				messageKey: "vibeRepeatMinutes",
				label: "Repeat (minutes)",
				defaultValue: 60,
				min: 15,
				max: 120,
				step: 15
			},
			{
				type: "text",
				defaultValue: "<small>Wait for delay before first alert, then repeat</small>"
			}
		]
	},
	{
		type: "section",
		items: [
			{
				type: "heading",
				defaultValue: "Saltie Integration"
			},
			{
				type: "input",
				messageKey: "saltieApiToken",
				label: "Saltie API Token",
				attributes: {
					placeholder: "Enter your Saltie API token",
					autocapitalize: "off",
					autocorrect: "off"
				}
			},
			{
				type: "text",
				defaultValue: "<small>Optional: Enter your Saltie API token to track meals</small>"
			}
		]
	},
	{
		type: "submit",
		defaultValue: "Save Settings"
	}
];
